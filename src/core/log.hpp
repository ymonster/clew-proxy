#pragma once

// Clew logging wrapper over quill.
// All source files use PC_LOG_* macros; logger pointer is global singleton.

#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/Logger.h"
#include "quill/sinks/ConsoleSink.h"
#include "quill/sinks/RotatingFileSink.h"

#include <string>

namespace clew {

// Global logger — set once in main(), used everywhere via macros below.
inline quill::Logger* g_logger = nullptr;

inline quill::LogLevel parse_log_level(const std::string& s) {
    if (s == "debug" || s == "trace") return quill::LogLevel::Debug;
    if (s == "info")    return quill::LogLevel::Info;
    if (s == "warning" || s == "warn") return quill::LogLevel::Warning;
    if (s == "error")   return quill::LogLevel::Error;
    return quill::LogLevel::Info;
}

inline void set_log_level(const std::string& level) {
    if (g_logger) g_logger->set_log_level(parse_log_level(level));
}

} // namespace clew

// ---- Convenience macros: hide the logger pointer ----
#define PC_LOG_DEBUG(fmt, ...)   LOG_DEBUG(clew::g_logger, fmt, ##__VA_ARGS__)
#define PC_LOG_INFO(fmt, ...)    LOG_INFO(clew::g_logger, fmt, ##__VA_ARGS__)
#define PC_LOG_WARN(fmt, ...)    LOG_WARNING(clew::g_logger, fmt, ##__VA_ARGS__)
#define PC_LOG_ERROR(fmt, ...)   LOG_ERROR(clew::g_logger, fmt, ##__VA_ARGS__)
