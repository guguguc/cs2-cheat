#pragma once

#include <cstdarg>
#include <mutex>

// Thread-safe process logger shared by the injected modules. Normal messages
// go to the file; errors also mirror to stderr for immediate diagnostics.
class Logger {
public:
    static Logger& instance();

    void log(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void error(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

private:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void write(bool mirror_stderr, const char* fmt, va_list args);

    std::mutex mutex_;
};
