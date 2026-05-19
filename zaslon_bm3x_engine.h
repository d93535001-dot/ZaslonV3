#pragma once

#include <string>
#include <vector>
#include <memory>
#include <map>
#include <chrono>
#include <mutex>
#include <optional>

namespace Zaslon {
namespace Engine {

// --------------------------------------------------------------------------
// Core Data Structures
// --------------------------------------------------------------------------

enum class RiskLevel {
    Safe,       // 0-20
    Suspicious, // 21-60
    Malicious,  // 61-90
    Critical    // 91-100
};

struct ProcessBehaviorEvent {
    uint32_t pid;
    uint32_t parent_pid;
    std::wstring image_path;
    std::wstring parent_image_path;
    std::wstring command_line;
    double pe_entropy; // Derived from Phase 3
    bool is_signed_valid;
    bool modifies_startup_registry;
    bool injects_memory;
    uint64_t timestamp;
};

struct MitigationAction {
    std::wstring action_type; // e.g., "Terminate", "Isolate", "Block Network"
    std::wstring description;
};

struct RiskAssessment {
    uint32_t pid;
    int risk_score; // 0 to 100
    RiskLevel level;
    std::vector<std::wstring> triggered_rules;
    std::vector<MitigationAction> suggested_actions;
};

// --------------------------------------------------------------------------
// BM-3X Active Defense Engine
// --------------------------------------------------------------------------
class BM3XEngine {
public:
    static BM3XEngine& GetInstance() {
        static BM3XEngine instance;
        return instance;
    }

    BM3XEngine(const BM3XEngine&) = delete;
    BM3XEngine& operator=(const BM3XEngine&) = delete;

    // Feeds a new behavioral event into the engine
    void AnalyzeEvent(const ProcessBehaviorEvent& event);

    // Retrieves the current risk assessment for a given PID
    std::optional<RiskAssessment> GetAssessment(uint32_t pid);

    // Retrieves all active high-risk threats
    std::vector<RiskAssessment> GetActiveThreats(RiskLevel threshold = RiskLevel::Malicious);

private:
    BM3XEngine() = default;
    ~BM3XEngine() = default;

    int CalculateScore(const ProcessBehaviorEvent& event, std::vector<std::wstring>& out_rules);
    RiskLevel ScoreToLevel(int score);
    std::vector<MitigationAction> GenerateMitigations(const ProcessBehaviorEvent& event, int score);

    // Context tracking for chaining events (e.g., rapid consecutive suspicious actions)
    struct ProcessContext {
        ProcessBehaviorEvent last_event;
        int accumulated_score = 0;
        std::vector<std::wstring> historical_rules;
    };

    std::map<uint32_t, ProcessContext> m_process_contexts;
    std::mutex m_engine_mutex;

    // Helper functions for heuristic rules
    bool IsSuspiciousParentChild(const std::wstring& parent, const std::wstring& child) const;
    bool IsSuspiciousCommandLine(const std::wstring& cmd) const;
};

} // namespace Engine
} // namespace Zaslon
