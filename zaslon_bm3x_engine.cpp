#include "zaslon_bm3x_engine.h"
#include "zaslon_logger.h"
#include <algorithm>

namespace Zaslon {
namespace Engine {

// Helper string lowercase function
static std::wstring ToLower(std::wstring str) {
    std::transform(str.begin(), str.end(), str.begin(), ::towlower);
    return str;
}

bool BM3XEngine::IsSuspiciousParentChild(const std::wstring& parent_path, const std::wstring& child_path) const {
    std::wstring p = ToLower(parent_path);
    std::wstring c = ToLower(child_path);

    // Rule 1: Office applications launching shells (Classic macro malware behavior)
    if ((p.find(L"winword.exe") != std::wstring::npos || p.find(L"excel.exe") != std::wstring::npos) &&
        (c.find(L"cmd.exe") != std::wstring::npos || c.find(L"powershell.exe") != std::wstring::npos || c.find(L"cscript.exe") != std::wstring::npos)) {
        return true;
    }

    // Rule 2: Browsers or PDF readers launching shells
    if ((p.find(L"chrome.exe") != std::wstring::npos || p.find(L"acrord32.exe") != std::wstring::npos) &&
        (c.find(L"cmd.exe") != std::wstring::npos || c.find(L"powershell.exe") != std::wstring::npos)) {
        return true;
    }

    // Rule 3: WMI launching shells directly
    if (p.find(L"wmiprvse.exe") != std::wstring::npos &&
       (c.find(L"cmd.exe") != std::wstring::npos || c.find(L"powershell.exe") != std::wstring::npos)) {
        return true;
    }

    return false;
}

bool BM3XEngine::IsSuspiciousCommandLine(const std::wstring& cmd) const {
    std::wstring c = ToLower(cmd);

    // Obfuscation / Execution bypass patterns
    if (c.find(L"-nop") != std::wstring::npos || c.find(L"-noprofile") != std::wstring::npos) {
        if (c.find(L"-enc") != std::wstring::npos || c.find(L"-encodedcommand") != std::wstring::npos || c.find(L"-w hidden") != std::wstring::npos) {
            return true; // Highly suspicious PowerShell execution
        }
    }

    // Living off the Land (LotL) abuse
    if (c.find(L"certutil") != std::wstring::npos && c.find(L"urlcache") != std::wstring::npos) return true;
    if (c.find(L"bitsadmin") != std::wstring::npos && c.find(L"transfer") != std::wstring::npos) return true;
    if (c.find(L"regsvr32") != std::wstring::npos && c.find(L"/i:http") != std::wstring::npos) return true; // Squiblydoo

    // Disabling security
    if (c.find(L"vssadmin") != std::wstring::npos && c.find(L"delete shadows") != std::wstring::npos) return true; // Ransomware behavior

    return false;
}

int BM3XEngine::CalculateScore(const ProcessBehaviorEvent& event, std::vector<std::wstring>& out_rules) {
    int score = 0;

    if (!event.is_signed_valid) {
        score += 15;
        out_rules.push_back(L"Unsigned executable");
    }

    if (event.pe_entropy > 7.2) {
        score += 25;
        out_rules.push_back(L"High PE Entropy (Packed/Encrypted)");
    }

    if (IsSuspiciousParentChild(event.parent_image_path, event.image_path)) {
        score += 45;
        out_rules.push_back(L"Suspicious Parent-Child Process Relationship");
    }

    if (IsSuspiciousCommandLine(event.command_line)) {
        score += 40;
        out_rules.push_back(L"Malicious Command Line Signature Match (LotL/Obfuscation)");
    }

    if (event.modifies_startup_registry) {
        score += 20;
        out_rules.push_back(L"Startup Registry Modification");
    }

    if (event.injects_memory) {
        score += 55;
        out_rules.push_back(L"Remote Thread Injection / Cross-Process Memory Write");
    }

    // Cap score at 100
    return std::min(score, 100);
}

RiskLevel BM3XEngine::ScoreToLevel(int score) {
    if (score >= 91) return RiskLevel::Critical;
    if (score >= 61) return RiskLevel::Malicious;
    if (score >= 21) return RiskLevel::Suspicious;
    return RiskLevel::Safe;
}

std::vector<MitigationAction> BM3XEngine::GenerateMitigations(const ProcessBehaviorEvent& event, int score) {
    std::vector<MitigationAction> actions;

    if (score >= 91) {
        actions.push_back({L"Force Terminate", L"Instantly kill the process and its children to prevent severe damage (e.g., Ransomware)."});
        actions.push_back({L"Quarantine File", L"Move the executable to a secure encrypted vault."});

        if (event.modifies_startup_registry) {
            actions.push_back({L"Revert Registry", L"Remove the persistence hooks added by this process."});
        }
    } else if (score >= 61) {
        actions.push_back({L"Suspend Process", L"Pause execution for manual administrator review."});
        actions.push_back({L"Block Network", L"Isolate process from network to prevent C2 beaconing."});
    }

    return actions;
}

void BM3XEngine::AnalyzeEvent(const ProcessBehaviorEvent& event) {
    std::lock_guard<std::mutex> lock(m_engine_mutex);

    std::vector<std::wstring> triggered_rules;
    int base_score = CalculateScore(event, triggered_rules);

    // Context tracking logic
    auto& ctx = m_process_contexts[event.pid];
    ctx.last_event = event;

    // Accumulate score over time, but decay it slightly if old (decay logic omitted for brevity, but accumulation is key)
    ctx.accumulated_score = std::min(ctx.accumulated_score + base_score, 100);

    for (const auto& rule : triggered_rules) {
        if (std::find(ctx.historical_rules.begin(), ctx.historical_rules.end(), rule) == ctx.historical_rules.end()) {
            ctx.historical_rules.push_back(rule);
        }
    }

    RiskLevel current_level = ScoreToLevel(ctx.accumulated_score);

    if (current_level >= RiskLevel::Malicious) {
        ZLOG_CRIT("BM-3X: Threat Detected! PID: {}, Score: {}, Path: {}",
            event.pid, ctx.accumulated_score, std::string(event.image_path.begin(), event.image_path.end()));
    } else if (current_level >= RiskLevel::Suspicious) {
        ZLOG_WARN("BM-3X: Suspicious activity from PID: {}, Score: {}", event.pid, ctx.accumulated_score);
    }
}

std::optional<RiskAssessment> BM3XEngine::GetAssessment(uint32_t pid) {
    std::lock_guard<std::mutex> lock(m_engine_mutex);

    auto it = m_process_contexts.find(pid);
    if (it != m_process_contexts.end()) {
        RiskAssessment assessment;
        assessment.pid = pid;
        assessment.risk_score = it->second.accumulated_score;
        assessment.level = ScoreToLevel(assessment.risk_score);
        assessment.triggered_rules = it->second.historical_rules;
        assessment.suggested_actions = GenerateMitigations(it->second.last_event, assessment.risk_score);
        return assessment;
    }
    return std::nullopt;
}

std::vector<RiskAssessment> BM3XEngine::GetActiveThreats(RiskLevel threshold) {
    std::lock_guard<std::mutex> lock(m_engine_mutex);
    std::vector<RiskAssessment> threats;

    for (const auto& [pid, ctx] : m_process_contexts) {
        RiskLevel lvl = ScoreToLevel(ctx.accumulated_score);
        if (lvl >= threshold) {
            RiskAssessment assessment;
            assessment.pid = pid;
            assessment.risk_score = ctx.accumulated_score;
            assessment.level = lvl;
            assessment.triggered_rules = ctx.historical_rules;
            assessment.suggested_actions = GenerateMitigations(ctx.last_event, assessment.risk_score);
            threats.push_back(assessment);
        }
    }

    return threats;
}

} // namespace Engine
} // namespace Zaslon
