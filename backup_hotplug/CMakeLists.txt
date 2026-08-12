cmake_minimum_required(VERSION 3.16)
project(cs2_cheat LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ImGui + GLFW + OpenGL3 overlay. Works on X11 and Wayland (GLFW backends);
# on Wayland-native sessions the terminal radar (default) is the reliable ESP.
option(USE_IMGUI "Build the ImGui/GLFW/OpenGL overlay" ON)
option(BUILD_INTERNAL "Build the injected cs2_internal.so (in-game Vulkan ImGui overlay)" ON)

add_executable(cs2_cheat
    src/main.cpp
    src/memory.cpp
    src/patterns.cpp
    src/process.cpp
    src/game.cpp
    src/hyprland.cpp
    src/aimbot.cpp
    src/trigger.cpp
    src/radar.cpp
    src/demo.cpp
)
target_include_directories(cs2_cheat PRIVATE src)
target_compile_options(cs2_cheat PRIVATE -Wall -Wextra -O2)

if(USE_IMGUI)
    find_package(glfw3 QUIET)
    if(NOT glfw3_FOUND)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(GLFW3 QUIET IMPORTED_TARGET glfw3)
        endif()
    endif()
    find_package(OpenGL QUIET)

    set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
    if((glfw3_FOUND OR TARGET PkgConfig::GLFW3) AND OpenGL_FOUND AND EXISTS ${IMGUI_DIR}/imgui.cpp)
        add_library(imgui STATIC
            ${IMGUI_DIR}/imgui.cpp
            ${IMGUI_DIR}/imgui_draw.cpp
            ${IMGUI_DIR}/imgui_tables.cpp
            ${IMGUI_DIR}/imgui_widgets.cpp
            ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
            ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
        target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
        if(TARGET PkgConfig::GLFW3)
            target_link_libraries(imgui PUBLIC PkgConfig::GLFW3)
        else()
            target_link_libraries(imgui PUBLIC glfw)
        endif()

        target_sources(cs2_cheat PRIVATE src/overlay_imgui.cpp)
        target_compile_definitions(cs2_cheat PRIVATE USE_IMGUI=1)
        target_link_libraries(cs2_cheat PRIVATE imgui OpenGL::GL)
    else()
        message(WARNING "USE_IMGUI is ON but glfw3/OpenGL/ImGui were not found; building without the overlay (terminal radar only)")
    endif()
endif()

# ---------------------------------------------------------------------------
# cs2_internal.so - injected shared object that renders the ImGui overlay
# inside the game's own swapchain (Vulkan present hook). No separate window.
# ---------------------------------------------------------------------------
if(BUILD_INTERNAL)
    find_package(Vulkan QUIET)
    find_package(X11 QUIET)
    if(Vulkan_FOUND AND X11_FOUND)
        add_library(cs2_internal SHARED
            internal/entry.cpp
            internal/uinput_aim.cpp
            internal/vk_hook.cpp
            internal/input_x11.cpp
            internal/overlay_draw.cpp
            src/patterns.cpp
            src/game.cpp
            src/memory.cpp
            src/process.cpp
            src/aimbot.cpp
            src/trigger.cpp
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
endif()

# Math sanity tests — pure, runs on any platform.
enable_testing()
add_executable(math_test tests/math_test.cpp)
target_include_directories(math_test PRIVATE src)
add_test(NAME math_test COMMAND math_test)

# Inline-hook/trampoline verification against the real Vulkan loader.
if(BUILD_INTERNAL AND Vulkan_FOUND)
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

# find_va - locate the live view-angle QAngle global in the running game by
# correlating candidate triples in libclient.so's data with the camera
# rotation derived from the view matrix. Host-side probe (external attach).
add_executable(find_va
    tools/find_va.cpp
    src/memory.cpp
    src/process.cpp
)
target_include_directories(find_va PRIVATE src)
target_compile_options(find_va PRIVATE -Wall -Wextra -O2)

add_executable(input_test
    tools/input_test.cpp
    src/memory.cpp
    src/process.cpp
)
target_include_directories(input_test PRIVATE src)
target_compile_options(input_test PRIVATE -Wall -Wextra -O2)

add_executable(find_camera_slot
    tools/find_camera_slot.cpp
    src/memory.cpp
    src/process.cpp
)
target_include_directories(find_camera_slot PRIVATE src)
target_compile_options(find_camera_slot PRIVATE -Wall -Wextra -O2)
