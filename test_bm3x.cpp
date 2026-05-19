#include "zaslon_logger.h"
#include "zaslon_bm3x_engine.h"
#include <iostream>

using namespace Zaslon::Engine;
using namespace Zaslon::Foundation;

void PrintAssessment(const std::optional<RiskAssessment>& assessment) {
    if (!assessment) {
        std::cout << "No assessment available." << std::endl;
        return;
    }

    std::wcout << L"\n--- Threat Assessment for PID: " << assessment->pid << L" ---" << std::endl;
    std::wcout << L"Score: " << assessment->risk_score << L"/100" << std::endl;

    std::wcout << L"Rules Triggered:" << std::endl;
    for (const auto& rule : assessment->triggered_rules) {
        std::wcout << L"  - " << rule << std::endl;
    }

    std::wcout << L"Suggested Actions:" << std::endl;
    for (const auto& action : assessment->suggested_actions) {
        std::wcout << L"  [!] " << action.action_type << L": " << action.description << std::endl;
    }
    std::wcout << L"---------------------------------------\n" << std::endl;
}

int main() {
    auto& logger = Logger::GetInstance();
    logger.InitializeFileSink(L"zaslon_bm3x_test.log");

    auto& engine = BM3XEngine::GetInstance();

    std::cout << "Testing BM-3X Active Defense Engine..." << std::endl;

    // Test Case 1: Normal Application
    ProcessBehaviorEvent ev1 = {};
    ev1.pid = 1000;
    ev1.parent_pid = 800;
    ev1.image_path = L"C:\\Program Files\\Browser\\browser.exe";
    ev1.parent_image_path = L"C:\\Windows\\explorer.exe";
    ev1.command_line = L"browser.exe";
    ev1.pe_entropy = 5.5;
    ev1.is_signed_valid = true;
    engine.AnalyzeEvent(ev1);

    // Test Case 2: Ransomware Behavior (Unsigned, Packed, modifying registry, disabling VSS)
    ProcessBehaviorEvent ev2 = {};
    ev2.pid = 666;
    ev2.parent_pid = 1000; // Launched from browser
    ev2.image_path = L"C:\\Users\\Admin\\AppData\\Local\\Temp\\invoice.exe";
    ev2.parent_image_path = L"C:\\Program Files\\Browser\\browser.exe";
    ev2.command_line = L"vssadmin.exe delete shadows /all /quiet";
    ev2.pe_entropy = 7.9;
    ev2.is_signed_valid = false;
    ev2.modifies_startup_registry = true;
    engine.AnalyzeEvent(ev2);

    // Test Case 3: Fileless / Macro malware (Word launching PowerShell encoded)
    ProcessBehaviorEvent ev3 = {};
    ev3.pid = 999;
    ev3.parent_pid = 555;
    ev3.image_path = L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
    ev3.parent_image_path = L"C:\\Program Files\\Microsoft Office\\root\\Office16\\WINWORD.EXE";
    ev3.command_line = L"powershell.exe -nop -w hidden -EncodedCommand JABzAD0ATgBlAHcALQBPAGIAagBlAGMAdAAgAEkATwAuAE0AZQBtAG8AcgB5AFMAdAByAGUAYQBtACgAWwBDAG8AbgB2AGUAcgB0AF0AOgA6AEYAcgBvAG0AQgBhAHMAZQA2ADQAUwB0AHIAaQBuAGcAKAAiAEgA...";
    ev3.pe_entropy = 6.0;
    ev3.is_signed_valid = true; // PS itself is signed
    engine.AnalyzeEvent(ev3);

    // Print Results
    PrintAssessment(engine.GetAssessment(1000));
    PrintAssessment(engine.GetAssessment(666));
    PrintAssessment(engine.GetAssessment(999));

    std::cout << "Checking active threats..." << std::endl;
    auto threats = engine.GetActiveThreats(RiskLevel::Malicious);
    std::cout << "Found " << threats.size() << " critical/malicious threats." << std::endl;

    return 0;
}
