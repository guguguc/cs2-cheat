#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Returns PIDs of processes whose /proc/<pid>/comm starts with `name`.
std::vector<int> find_processes(const std::string& name);

// Lowest mapping start for the module whose backing path ends with `module`,
// parsed from /proc/<pid>/maps. Works for both the native Linux build
// (libclient.so) and Windows-under-Proton (client.dll).
std::optional<std::uintptr_t> module_base(int pid, const std::string& module);

// /proc/sys/kernel/yama/ptrace_scope (0 = anyone, 1 = descendants only, ...).
int ptrace_scope();
