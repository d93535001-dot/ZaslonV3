#include "zaslon_logger.h"
#include <iostream>

#ifdef _WIN32
#include <winmeta.h>
#pragma comment(lib, "Advapi32.lib")
#endif

namespace Zaslon {
namespace Foundation {

#ifdef _WIN32
Logger::Logger() : m_etw_handle(0), m_etw_registered(false) {
    // Initialize ETW Provider natively
    ULONG status = EventRegister(&ZASLON_ETW_PROVIDER, nullptr, nullptr, &m_etw_handle);
    if (status == ERROR_SUCCESS) {
        m_etw_registered = true;
    }
}
#else
Logger::Logger() {}
#endif

Logger::~Logger() {
    Flush();
    if (m_file_stream.is_open()) {
        m_file_stream.close();
    }

#ifdef _WIN32
    if (m_etw_registered) {
        EventUnregister(m_etw_handle);
        m_etw_registered = false;
    }
#endif
}

bool Logger::InitializeFileSink(const std::wstring& file_path) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_file_stream.is_open()) {
        m_file_stream.close();
    }

    // Open file in append mode. Use RAII via std::ofstream
#ifdef _WIN32
    m_file_stream.open(file_path, std::ios::out | std::ios::app);
#else
    m_file_stream.open(std::string(file_path.begin(), file_path.end()), std::ios::out | std::ios::app);
#endif
    return m_file_stream.is_open();
}

void Logger::Flush() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file_stream.is_open()) {
        m_file_stream.flush();
    }
}

std::string Logger::GetTimestamp() const {
    auto now = std::chrono::system_clock::now();
    // std::format supports chrono formatting in C++20
    return std::format("{:%Y-%m-%d %H:%M:%OS}", now);
}

std::string Logger::LevelToString(LogLevel level) const {
    switch (level) {
        case LogLevel::Info:     return "INFO";
        case LogLevel::Warning:  return "WARN";
        case LogLevel::Error:    return "ERROR";
        case LogLevel::Critical: return "CRIT";
        case LogLevel::Audit:    return "AUDIT";
        default:                 return "UNKNOWN";
    }
}

#ifdef _WIN32
UCHAR Logger::LevelToEtwLevel(LogLevel level) const {
    switch (level) {
        case LogLevel::Critical: return WINEVENT_LEVEL_CRITICAL;
        case LogLevel::Error:    return WINEVENT_LEVEL_ERROR;
        case LogLevel::Warning:  return WINEVENT_LEVEL_WARNING;
        case LogLevel::Info:     return WINEVENT_LEVEL_INFO;
        case LogLevel::Audit:    return WINEVENT_LEVEL_LOG_ALWAYS;
        default:                 return WINEVENT_LEVEL_INFO;
    }
}
#endif

void Logger::WriteLog(LogLevel level, std::string_view message) {
    std::string timestamp = GetTimestamp();
    std::string level_str = LevelToString(level);

    // Construct the final log line
    std::string log_line = std::format("[{}] [{}] {}\n", timestamp, level_str, message);

    // 1. Thread-safe write to Local File Sink
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_file_stream.is_open()) {
            m_file_stream << log_line;
            // For critical or audit events, flush immediately to prevent data loss on crash
            if (level == LogLevel::Critical || level == LogLevel::Audit) {
                m_file_stream.flush();
            }
        }
    }

#ifdef _WIN32
    // 2. Thread-safe (implicitly by OS) write to ETW Sink
    if (m_etw_registered) {
        // EventWriteString is a simple, manifest-free way to write strings to ETW
        // It requires a Unicode string (LPCWSTR)

        // Convert UTF-8 std::string to UTF-16 std::wstring for ETW
        int size_needed = MultiByteToWideChar(CP_UTF8, 0, log_line.c_str(), (int)log_line.length(), NULL, 0);
        if (size_needed > 0) {
            std::wstring w_log_line(size_needed, 0);
            MultiByteToWideChar(CP_UTF8, 0, log_line.c_str(), (int)log_line.length(), &w_log_line[0], size_needed);

            // Write to ETW
            EventWriteString(m_etw_handle, LevelToEtwLevel(level), 0, w_log_line.c_str());
        }
    }
#endif
}

} // namespace Foundation
} // namespace Zaslon
