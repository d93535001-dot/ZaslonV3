#include "zaslon_logger.h"
#include "zaslon_deep_enhancement.h"
#include <iostream>

using namespace Zaslon::Enhancement;
using namespace Zaslon::Foundation;

void RunTests() {
    // 1. Test PE Inspector (Testing on current executable)
    ZLOG_INFO("Testing PE Inspector...");
    auto peInfo = PEInspector::AnalyzePEFile(L"test_enhancement.exe");
    if (peInfo) {
        ZLOG_INFO("PE Valid: {}, 64-bit: {}", peInfo->is_valid_pe, peInfo->is_64bit);
        ZLOG_INFO("Entropy: {:.2f}, Suspicious: {}", peInfo->overall_entropy, peInfo->has_suspicious_sections);
    } else {
        ZLOG_ERROR("PE Analysis failed.");
    }

    // 2. Test Persistence Hunter
    ZLOG_INFO("Testing Persistence Hunter...");
    auto appInit = PersistenceHunter::ScanAppInitDLLs();
    ZLOG_INFO("AppInit_DLLs found: {}", appInit.size());
    for (const auto& entry : appInit) {
        ZLOG_WARN("AppInit: {}", std::string(entry.target.begin(), entry.target.end()));
    }

    auto wmi = PersistenceHunter::ScanWMIPersistence();
    ZLOG_INFO("WMI Consumers found: {}", wmi.size());
    for (const auto& entry : wmi) {
        ZLOG_WARN("WMI: {}", std::string(entry.target.begin(), entry.target.end()));
    }

    // 3. Test Hardware Telemetry
    ZLOG_INFO("Testing Hardware Telemetry (Ring 3)...");
    auto hw = HardwareTelemetry::GatherMetricsRing3();
    ZLOG_INFO("Available RAM: {} bytes", hw.available_physical_memory);
    ZLOG_INFO("Active Handles (NTAPI): {}", hw.active_handles);
    ZLOG_INFO("Hooks Detected: {}", hw.possible_hook_detected);
}

int main() {
    auto& logger = Logger::GetInstance();
    logger.InitializeFileSink(L"zaslon_enhancement_test.log");

    std::cout << "Running Enhancement module tests..." << std::endl;
    RunTests();

    std::cout << "\nTests complete. Check log file." << std::endl;
    return 0;
}
