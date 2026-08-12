#include "memory.h"

#include <sys/uio.h>

#include <cstdio>

bool Memory::attach(int pid) {
    pid_ = pid;
    return pid_ > 0;
}

bool Memory::read(std::uintptr_t addr, void* buf, std::size_t size) const {
    if (pid_ <= 0 || size == 0) return false;
    const iovec local{ buf, size };
    const iovec remote{ reinterpret_cast<void*>(addr), size };
    return process_vm_readv(pid_, &local, 1, &remote, 1, 0) ==
           static_cast<ssize_t>(size);
}

bool Memory::write(std::uintptr_t addr, const void* buf, std::size_t size) const {
    if (pid_ <= 0 || size == 0) return false;
    const iovec local{ const_cast<void*>(buf), size };
    const iovec remote{ reinterpret_cast<void*>(addr), size };
    return process_vm_writev(pid_, &local, 1, &remote, 1, 0) ==
           static_cast<ssize_t>(size);
}
