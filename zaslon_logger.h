#pragma once

#ifdef _WIN32
#include <windows.h>
#include <evntprov.h>
#endif

#include <string>
#include <string_view>
#include <mutex>
#include <fstream>
#include <format>
#include <memory>
#include <chrono>

namespace Zaslon {
namespace Foundation {

enum class LogLevel {
    Info,
    Warning,
    Error,
    Critical,
    Audit
};

#ifdef _WIN32
// Custom ETW Provider GUID for ZASLON
// {A0B1C2D3-E4F5-6789-0123-456789ABCDEF} (Example GUID, replace in production)
// Note: We use EventWriteString for zero-dependency generic ETW logging,
// avoiding the need for a compiled manifest (MC.exe) for phase 1.
constexpr GUID ZASLON_ETW_PROVIDER =
{ 0xa0b1c2d3, 0xe4f5, 0x6789, { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef } };
#endif

class Logger {
public:
    // Meyers Singleton for safe, lazy initialization
    static Logger& GetInstance() {
        static Logger instance;
        return instance;
    }

    // Delete copy and move semantics
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    // Core logging template utilizing std::format
    template <typename... Args>
    void Log(LogLevel level, std::format_string<Args...> fmt, Args&&... args) {
        std::string formatted_message;
        try {
            formatted_message = std::format(fmt, std::forward<Args>(args)...);
        } catch (const std::format_error& e) {
            // Fallback if formatting fails
            formatted_message = std::string("Format error: ") + e.what();
        }

        WriteLog(level, formatted_message);
    }

    // Initialize file sink (e.g., C:\ZaslonLogs\zaslon_telemetry.log)
    bool InitializeFileSink(const std::wstring& file_path);
    void Flush();

private:
    Logger();
    ~Logger();

    void WriteLog(LogLevel level, std::string_view message);
    std::string GetTimestamp() const;
    std::string LevelToString(LogLevel level) const;
#ifdef _WIN32
    UCHAR LevelToEtwLevel(LogLevel level) const;
#endif

    // Thread-safety
    mutable std::mutex m_mutex; // Or std::shared_mutex if reads become necessary

    // File Sink
    std::ofstream m_file_stream;

#ifdef _WIN32
    // ETW Sink
    REGHANDLE m_etw_handle;
    bool m_etw_registered;
#endif
};

// Helper macros for cleaner usage in code
#define ZLOG_INFO(...)    Zaslon::Foundation::Logger::GetInstance().Log(Zaslon::Foundation::LogLevel::Info, __VA_ARGS__)
#define ZLOG_WARN(...)    Zaslon::Foundation::Logger::GetInstance().Log(Zaslon::Foundation::LogLevel::Warning, __VA_ARGS__)
#define ZLOG_ERROR(...)   Zaslon::Foundation::Logger::GetInstance().Log(Zaslon::Foundation::LogLevel::Error, __VA_ARGS__)
#define ZLOG_CRIT(...)    Zaslon::Foundation::Logger::GetInstance().Log(Zaslon::Foundation::LogLevel::Critical, __VA_ARGS__)
#define ZLOG_AUDIT(...)   Zaslon::Foundation::Logger::GetInstance().Log(Zaslon::Foundation::LogLevel::Audit, __VA_ARGS__)

} // namespace Foundation
} // namespace Zaslon
