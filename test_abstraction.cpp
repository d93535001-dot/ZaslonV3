#include "zaslon_logger.h"
#include "zaslon_os_abstraction.h"
#include <iostream>

using namespace Zaslon::Abstraction;
using namespace Zaslon::Foundation;

void RunTests(bool is_winpe) {
    auto& ctx = EnvironmentContext::GetInstance();
    // Simulate target OS is on D: in WinPE mode
    ctx.Initialize(is_winpe, is_winpe ? L"D:" : L"");

    IOSFactory* factory = ctx.GetFactory();
    if (!factory) {
        ZLOG_ERROR("Factory initialization failed!");
        return;
    }

    auto fsManager = factory->CreateFileSystemManager();
    auto regManager = factory->CreateRegistryManager();
    auto procManager = factory->CreateProcessManager();

    ZLOG_INFO("Running tests in {} mode.", is_winpe ? "WinPE Offline" : "Live");

    // Test File System
    std::wstring testPath = L"C:\\Windows\\System32\\cmd.exe";
    bool exists = fsManager->FileExists(testPath);
    ZLOG_INFO("FileExists({}): {}", std::string(testPath.begin(), testPath.end()), exists);

    // Test Registry
    regManager->ReadStringValue(L"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ProductName");

    // Test Process
    auto procs = procManager->EnumerateProcesses();
    ZLOG_INFO("Enumerated {} processes.", procs.size());
}

int main() {
    auto& logger = Logger::GetInstance();
    logger.InitializeFileSink(L"zaslon_abstraction_test.log");

    std::cout << "Running LIVE mode test..." << std::endl;
    RunTests(false);

    std::cout << "\nRunning WINPE OFFLINE mode test..." << std::endl;
    RunTests(true);

    std::cout << "\nTests complete. Check log file." << std::endl;
    return 0;
}
