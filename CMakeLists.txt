cmake_minimum_required(VERSION 3.16)
project(cs2_cheat LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# cs2_internal.so - injected shared object: in-game Vulkan ImGui overlay
# (swapchain present hook), uinput virtual-mouse aimbot and triggerbot.
# This is the only production target; the old external window/radar tool was
# removed.
# ---------------------------------------------------------------------------
find_package(Vulkan QUIET)
find_package(X11 QUIET)
if(Vulkan_FOUND AND X11_FOUND)
    add_library(cs2_internal SHARED
        internal/entry.cpp
        internal/imgui_renderer.cpp
        internal/mouse_device.cpp
        internal/vk_hook.cpp
        internal/input_x11.cpp
        internal/overlay_draw.cpp
        src/cfg.cpp
        src/patterns.cpp
        src/game.cpp
        src/memory.cpp
        src/process.cpp
        src/aimbot.cpp
        src/trigger.cpp
        src/bvh.cpp
        src/rcs.cpp
    )
    target_include_directories(cs2_internal PRIVATE src internal)
    # -fcf-protection: gives our detours endbr64 prologues so indirect
    # jumps into them stay CET/IBT-compliant if the game enforces IBT.
    # NDEBUG: ImGui's IM_ASSERT calls abort() on failure (the game was
    # crashing on a failed vk call); with NDEBUG assertions are no-ops and
    # failures are just logged.
    target_compile_options(cs2_internal PRIVATE -Wall -Wextra -O2 -fcf-protection -DNDEBUG)

    set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
    target_sources(cs2_internal PRIVATE
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
    )
    target_include_directories(cs2_internal PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
    target_link_libraries(cs2_internal PRIVATE Vulkan::Vulkan X11::X11 Xi dl pthread)
else()
    message(WARNING "BUILD_INTERNAL is ON but Vulkan/X11 dev files were not found")
endif()

# ---------------------------------------------------------------------------
# Tests (pure, run on any platform)
# ---------------------------------------------------------------------------
enable_testing()
add_executable(math_test tests/math_test.cpp)
target_include_directories(math_test PRIVATE src)
add_test(NAME math_test COMMAND math_test)

if(Vulkan_FOUND)
    add_executable(vk_hook_selftest tests/vk_hook_selftest.cpp)
    target_include_directories(vk_hook_selftest PRIVATE internal)
    target_link_libraries(vk_hook_selftest PRIVATE Vulkan::Vulkan dl)
    add_test(NAME vk_hook_selftest COMMAND vk_hook_selftest)

    add_executable(scan_test tests/scan_test.cpp)
    target_link_libraries(scan_test PRIVATE dl)
    add_test(NAME scan_test COMMAND scan_test)

    add_executable(hook64_test tests/hook64_test.cpp)
    target_include_directories(hook64_test PRIVATE internal)
    target_compile_options(hook64_test PRIVATE -Wall -Wextra -O2)
endif()
