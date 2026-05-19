#include "zaslon_kernel_proxy.h"
#include "zaslon_logger.h"
#include <iostream>
#include <cstring>

#ifdef _WIN32
#include <winioctl.h>
#endif

#ifndef _WIN32
// Stub Linux implementations for testing
static DWORD GetLastError() { return 2; } // ERROR_FILE_NOT_FOUND
static HANDLE CreateFileW(...) { return INVALID_HANDLE_VALUE; }
static void CloseHandle(HANDLE) {}
static uint32_t GetCurrentProcessId() { return 1234; }
static BOOL DeviceIoControl(...) { return 0; }
static HANDLE CreateEventW(...) { return (HANDLE)0x1234; }
static void SetEvent(HANDLE) {}
static void CancelIo(HANDLE) {}
static void ResetEvent(HANDLE) {}
static DWORD WaitForSingleObject(HANDLE, DWORD) { return 0; }
static BOOL GetOverlappedResult(...) { return 0; }
#define ERROR_IO_PENDING 997
#define WAIT_OBJECT_0 0
#define INFINITE 0xFFFFFFFF
#endif

namespace Zaslon {
namespace Kernel {

KernelDriverProxy::KernelDriverProxy()
    : m_driver_handle(INVALID_HANDLE_VALUE),
      m_is_running(false),
      m_event_handle(NULL) {
    memset(&m_overlapped, 0, sizeof(OVERLAPPED));
}

KernelDriverProxy::~KernelDriverProxy() {
    Shutdown();
}

bool KernelDriverProxy::Initialize() {
#ifdef _WIN32
    m_driver_handle = CreateFileW(
        DEVICE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        NULL
    );
#else
    m_driver_handle = INVALID_HANDLE_VALUE;
#endif

    if (m_driver_handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        ZLOG_WARN("KernelDriverProxy: Failed to open device handle. Driver may not be loaded. Error: {}", error);
        return false;
    }

    if (!AuthenticateWithDriver()) {
        ZLOG_ERROR("KernelDriverProxy: Driver authentication failed. Security violation.");
        CloseHandle(m_driver_handle);
        m_driver_handle = INVALID_HANDLE_VALUE;
        return false;
    }

#ifdef _WIN32
    m_event_handle = CreateEventW(NULL, TRUE, FALSE, NULL);
#else
    m_event_handle = (HANDLE)0x1234;
#endif

    if (!m_event_handle) {
        ZLOG_ERROR("KernelDriverProxy: Failed to create async event.");
        return false;
    }
    m_overlapped.hEvent = m_event_handle;

    m_is_running = true;
    m_listener_thread = std::thread(&KernelDriverProxy::TelemetryListenerThread, this);

    ZLOG_INFO("KernelDriverProxy: Successfully linked to Ring 0 Engine.");
    return true;
}

void KernelDriverProxy::Shutdown() {
    if (m_is_running) {
        m_is_running = false;

        if (m_event_handle) {
#ifdef _WIN32
            SetEvent(m_event_handle);
#endif
        }

        if (m_listener_thread.joinable()) {
            m_listener_thread.join();
        }

        if (m_driver_handle != INVALID_HANDLE_VALUE) {
#ifdef _WIN32
            CancelIo(m_driver_handle);
            CloseHandle(m_driver_handle);
#endif
            m_driver_handle = INVALID_HANDLE_VALUE;
        }

        if (m_event_handle) {
#ifdef _WIN32
            CloseHandle(m_event_handle);
#endif
            m_event_handle = NULL;
        }

        ZLOG_INFO("KernelDriverProxy: Shutdown complete.");
    }
}

bool KernelDriverProxy::AuthenticateWithDriver() {
    AuthenticationRequest req = {0};
    req.magic_header = 0x5A41534C; // 'ZASL'

#ifdef _WIN32
    req.process_id = GetCurrentProcessId();
#else
    req.process_id = 1234;
#endif

    memset(req.signature_hash, 0xAA, sizeof(req.signature_hash));

    DWORD bytesReturned = 0;

#ifdef _WIN32
    BOOL result = DeviceIoControl(
        m_driver_handle,
        IOCTL_ZASLON_AUTHENTICATE,
        &req, sizeof(req),
        NULL, 0,
        &bytesReturned,
        NULL
    );
#else
    BOOL result = 1;
#endif

    return result != 0;
}

bool KernelDriverProxy::TerminateProcess(uint32_t pid) {
    if (m_driver_handle == INVALID_HANDLE_VALUE) return false;

    DWORD bytesReturned = 0;

#ifdef _WIN32
    BOOL result = DeviceIoControl(
        m_driver_handle,
        IOCTL_ZASLON_TERMINATE_PID,
        &pid, sizeof(pid),
        NULL, 0,
        &bytesReturned,
        NULL
    );
#else
    BOOL result = 1;
#endif

    if (result) {
        ZLOG_AUDIT("KernelDriverProxy: Successfully terminated PID {} via Ring 0.", pid);
    } else {
        ZLOG_ERROR("KernelDriverProxy: Ring 0 termination failed for PID {}. Error: {}", pid, GetLastError());
    }

    return result != 0;
}

void KernelDriverProxy::SetTelemetryCallback(TelemetryCallback callback) {
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    m_telemetry_callback = callback;
}

void KernelDriverProxy::TelemetryListenerThread() {
    const DWORD bufferSize = 4096;
    std::vector<uint8_t> buffer(bufferSize);

    while (m_is_running) {
        DWORD bytesReturned = 0;

#ifdef _WIN32
        ResetEvent(m_event_handle);
        BOOL bResult = DeviceIoControl(
            m_driver_handle,
            IOCTL_ZASLON_GET_TELEMETRY,
            NULL, 0,
            buffer.data(), bufferSize,
            &bytesReturned,
            &m_overlapped
        );

        if (!bResult) {
            DWORD error = GetLastError();
            if (error == ERROR_IO_PENDING) {
                DWORD waitResult = WaitForSingleObject(m_event_handle, INFINITE);

                if (!m_is_running) break;

                if (waitResult == WAIT_OBJECT_0) {
                    if (GetOverlappedResult(m_driver_handle, &m_overlapped, &bytesReturned, FALSE)) {
                        if (bytesReturned >= sizeof(TelemetryEventHeader)) {
                            auto* header = reinterpret_cast<TelemetryEventHeader*>(buffer.data());
                            const uint8_t* payload = buffer.data() + sizeof(TelemetryEventHeader);

                            std::lock_guard<std::mutex> lock(m_callback_mutex);
                            if (m_telemetry_callback) {
                                m_telemetry_callback(header, payload);
                            }
                        }
                    }
                }
            } else {
                ZLOG_ERROR("KernelDriverProxy: DeviceIoControl failed unexpectedly. Error: {}", error);
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            }
        }
#else
        // Mock listener logic for testing
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        break;
#endif
    }
}

} // namespace Kernel
} // namespace Zaslon
