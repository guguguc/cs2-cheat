#include "logger.h"

#include <cstdio>

namespace {
constexpr const char* kLogPath = "/tmp/cs2_internal.log";
}

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::write(bool mirror_stderr, const char* fmt, va_list args) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (FILE* file = std::fopen(kLogPath, "a")) {
        va_list copy;
        va_copy(copy, args);
        std::vfprintf(file, fmt, copy);
        va_end(copy);
        std::fflush(file);
        std::fclose(file);
    }

    if (mirror_stderr) {
        va_list copy;
        va_copy(copy, args);
        std::vfprintf(stderr, fmt, copy);
        va_end(copy);
        std::fflush(stderr);
    }
}

void Logger::log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    write(false, fmt, args);
    va_end(args);
}

void Logger::error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    write(true, fmt, args);
    va_end(args);
}
