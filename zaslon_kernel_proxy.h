#pragma once

#ifdef _WIN32
#include <windows.h>
#else
// Stub definitions for Linux environment so we can syntax check the C++ code
#include <cstdint>
typedef void* HANDLE;
typedef unsigned long DWORD;
typedef int BOOL;
typedef struct _OVERLAPPED { void* hEvent; } OVERLAPPED;
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define METHOD_BUFFERED 0
#define FILE_ANY_ACCESS 0
#define CTL_CODE( DeviceType, Function, Method, Access ) (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))
#endif

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <mutex>
#include <thread>
#include <atomic>

namespace Zaslon {
namespace Kernel {

constexpr auto DEVICE_NAME = L"\\\\.\\ZaslonSecurityEngine";

#define ZASLON_DEVICE_TYPE 0x8000

#define IOCTL_ZASLON_AUTHENTICATE      CTL_CODE(ZASLON_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_ZASLON_GET_TELEMETRY     CTL_CODE(ZASLON_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_ZASLON_TERMINATE_PID     CTL_CODE(ZASLON_DEVICE_TYPE, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

#pragma pack(push, 1)

struct AuthenticationRequest {
    uint32_t magic_header;
    uint32_t process_id;
    uint8_t  signature_hash[32];
};

enum class TelemetryEventType : uint32_t {
    ProcessCreate = 1,
    ProcessTerminate = 2,
    ImageLoad = 3,
    RegistryWrite = 4
};

struct TelemetryEventHeader {
    TelemetryEventType type;
    uint32_t event_size;
    uint64_t timestamp;
};

struct ProcessCreateEvent {
    uint32_t parent_pid;
    uint32_t child_pid;
    wchar_t image_path[260];
    wchar_t command_line[512];
};

#pragma pack(pop)

class KernelDriverProxy {
public:
    static KernelDriverProxy& GetInstance() {
        static KernelDriverProxy instance;
        return instance;
    }

    KernelDriverProxy(const KernelDriverProxy&) = delete;
    KernelDriverProxy& operator=(const KernelDriverProxy&) = delete;

    bool Initialize();
    void Shutdown();
    bool TerminateProcess(uint32_t pid);

    using TelemetryCallback = std::function<void(const TelemetryEventHeader*, const uint8_t*)>;
    void SetTelemetryCallback(TelemetryCallback callback);

private:
    KernelDriverProxy();
    ~KernelDriverProxy();

    bool AuthenticateWithDriver();
    void TelemetryListenerThread();

    HANDLE m_driver_handle;
    std::atomic<bool> m_is_running;
    std::thread m_listener_thread;
    TelemetryCallback m_telemetry_callback;
    std::mutex m_callback_mutex;

    OVERLAPPED m_overlapped;
    HANDLE m_event_handle;
};

} // namespace Kernel
} // namespace Zaslon
