#include "zaslon_logger.h"
#include "zaslon_kernel_proxy.h"
#include <iostream>

using namespace Zaslon::Kernel;
using namespace Zaslon::Foundation;

void PrintTelemetryEvent(const TelemetryEventHeader* header, const uint8_t* payload) {
    if (!header) return;

    if (header->type == TelemetryEventType::ProcessCreate) {
        auto* ev = reinterpret_cast<const ProcessCreateEvent*>(payload);
        std::wcout << L"🔔 [RING 0 EVENT] Process Created! Parent PID: " << ev->parent_pid
                   << L", Child PID: " << ev->child_pid
                   << L", Path: " << ev->image_path << std::endl;
        ZLOG_INFO("Ring 0 Telemetry: Process Creation -> PID: {}", ev->child_pid);
    } else {
        ZLOG_WARN("Ring 0 Telemetry: Unknown event type received.");
    }
}

int main() {
    auto& logger = Logger::GetInstance();
    logger.InitializeFileSink(L"zaslon_kernel_proxy_test.log");

    std::cout << "Testing KernelDriverProxy initialization..." << std::endl;

    auto& proxy = KernelDriverProxy::GetInstance();

    proxy.SetTelemetryCallback(PrintTelemetryEvent);

    // Initialization will fail because we don't have a real .sys driver loaded
    // but we can observe the correct sequence and error handling.
    bool success = proxy.Initialize();

    if (success) {
        std::cout << "Initialization succeeded (Unexpected, driver should not be present)." << std::endl;
    } else {
        std::cout << "Initialization failed as expected (No driver loaded). Check logs for graceful handling." << std::endl;
    }

    // Try a termination command (Should also gracefully fail)
    proxy.TerminateProcess(1234);

    proxy.Shutdown();

    std::cout << "Tests complete." << std::endl;
    return 0;
}
