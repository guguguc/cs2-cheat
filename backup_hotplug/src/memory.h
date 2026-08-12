#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

// process_vm_readv / process_vm_writev based memory access. Works on the
// native Linux CS2 process (and on Proton, since Wine processes are ordinary
// Linux processes). No ptrace() attach needed, but the Yama ptrace scope still
// applies (see ptrace_scope()).
class Memory {
public:
    bool attach(int pid);
    int pid() const { return pid_; }
    bool attached() const { return pid_ > 0; }

    bool read(std::uintptr_t addr, void* buf, std::size_t size) const;
    bool write(std::uintptr_t addr, const void* buf, std::size_t size) const;

    template <typename T>
    std::optional<T> read(std::uintptr_t addr) const {
        T v{};
        if (!read(addr, &v, sizeof(T))) return std::nullopt;
        return v;
    }

    template <typename T>
    bool write(std::uintptr_t addr, const T& v) const {
        return write(addr, &v, sizeof(T));
    }

private:
    int pid_ = -1;
};
