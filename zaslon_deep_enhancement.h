#pragma once

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <cstdint>

namespace Zaslon {
namespace Enhancement {

// --------------------------------------------------------------------------
// Advanced PE (Portable Executable) Inspector
// --------------------------------------------------------------------------
struct PEInfo {
    bool is_valid_pe = false;
    bool is_64bit = false;
    uint32_t image_base = 0;
    uint32_t entry_point = 0;
    uint16_t subsystem = 0;
    std::vector<std::string> imported_dlls;
    std::vector<std::string> exported_functions;
    double overall_entropy = 0.0;
    bool has_suspicious_sections = false;
};

class PEInspector {
public:
    static std::optional<PEInfo> AnalyzePEFile(const std::wstring& file_path);
private:
    static double CalculateEntropy(const uint8_t* data, size_t size);
};

// --------------------------------------------------------------------------
// Persistence Hunter (WMI & AppInit_DLLs)
// --------------------------------------------------------------------------
struct PersistenceEntry {
    std::wstring type;
    std::wstring location;
    std::wstring target;
    std::wstring description;
};

class PersistenceHunter {
public:
    static std::vector<PersistenceEntry> ScanAppInitDLLs();
    static std::vector<PersistenceEntry> ScanWMIPersistence();
};

// --------------------------------------------------------------------------
// Hardware Telemetry (Ring 3 Fallback / NTAPI spoofing)
// --------------------------------------------------------------------------
struct HardwareMetrics {
    double cpu_usage_percent = 0.0;
    uint64_t available_physical_memory = 0;
    uint32_t active_handles = 0;
    bool possible_hook_detected = false;
};

class HardwareTelemetry {
public:
    static HardwareMetrics GatherMetricsRing3();
};

} // namespace Enhancement
} // namespace Zaslon
