# cs2-cheat

Injected ESP / aim assist for **Counter-Strike 2, native Linux build**.
A shared object (`libcs2_internal.so`) is injected into the running game; it
hooks the Vulkan loader and draws the ImGui ESP directly into the game's
swapchain image (no second window), reads the game's memory from userspace,
and aims through a **uinput virtual mouse** (so view-angle writes are not
needed and recoil compensation works like a real mouse).

> Research / education project. Using this against Valve's official servers
> violates the CS2 terms of service and will get you **VAC banned**. Use it on
> private / offline research setups only.

## Features

| Feature | Implementation |
|---|---|
| ESP: box + health bar + skeleton + head circle | ImGui drawn into the game's Vulkan swapchain (`internal/vk_hook.cpp` + `internal/overlay_draw.cpp`) |
| ESP visibility colors | per-player BVH line-of-sight; visible = red, hidden = blue |
| Aimbot (FOV-limited, adaptive smoothing + snap zone, inertia, BVH visibility check, **in-aimbot recoil comp**) | uinput virtual mouse (TI-84 disguise), targets the head bone |
| Triggerbot | delayed fire via uinput (flash / speed / head-only checks) |
| Standalone RCS | independent recoil compensation (mutually exclusive with the aimbot's in-aimbot recoil comp) |
| Offset resolution | Osiris-style pattern scans on the live binary + JSON config (`config/cs2_config.json`) |
| Unified logging | thread-safe `Logger` to `/tmp/cs2_internal.log` |
| Unload | remote `unload_self` + `injector`-based unloader tool (restores hooks, releases X11 grabs) |

## Hooked Vulkan functions

The SO installs four inline hooks. `vkCreateInstance` / `vkCreateDevice` are
patched on the loader stubs; because the game calls the real ICD functions
directly (cached pointers bypass the loader), `vkQueuePresentKHR` and
`vkCreateSwapchainKHR` are patched **in the ICD code itself** (each preserved
`endbr64` keeps indirect calls IBT/CET-compliant).

| Function | Patched at | Detour | Purpose |
|---|---|---|---|
| `vkCreateInstance` | loader stub | `detour_create_instance` | capture the `VkInstance` |
| `vkCreateDevice` | loader stub | `detour_create_device` | capture physical device / device / queue family |
| `vkQueuePresentKHR` | real ICD (inline) | `detour_present` | render the ImGui overlay before presenting |
| `vkCreateSwapchainKHR` | real ICD (inline) | `detour_create_swapchain_game` | (re)init the overlay when the game rebuilds its swapchain |

The hook machinery itself (`hook64::Hook`, trampoline allocation + RIP-reloc)
lives in `internal/hook_x64.{h,cpp}` and is verified by `hook64_test`.

## Build

Requirements: a C++20 compiler, CMake ≥ 3.16, Vulkan and X11 dev packages
(ImGui is vendored in `third_party/imgui`, MIT license).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Produces `build/libcs2_internal.so` plus selftests
(`./build/vk_hook_selftest`, `./build/hook64_test`, `./build/scan_test`,
`./build/math_test`).

## Inject & run

```sh
# your favourite SO injector, e.g. the kubo/injector-based one in this repo:
./injector -n cs2 libcs2_internal.so
```

On load the library:

1. spawns a data thread that pattern-scans the live binary and resolves
   offsets (paths/config in `config/cs2_config.json`, `CS2_CONFIG` env
   overrides the path),
2. inline-hooks `vkCreateInstance` / `vkCreateDevice` on the loader and the
   real `vkQueuePresentKHR` / `vkCreateSwapchainKHR` in the ICD (see the
   "Hooked Vulkan functions" table),
3. initializes the ImGui overlay on the next swapchain (re)creation,
4. opens an X11 connection to find the game window (`_NET_WM_PID`, recursive
   tree walk — required on GNOME/Mutter which reparents X11 windows),
5. lazily creates the uinput virtual mouse on first aim (kept alive).

Controls:

- `F1` (or `p`) — toggle the menu panel
- hold `x` — aimbot (key grabbed globally via XGrabKey)
- menu toggles ESP / aimbot / triggerbot / RCS, FOV, smooth, etc.

Progress/errors go to `/tmp/cs2_internal.log` (single unified log, see
`src/logger.cpp`).

### Unload (so you can re-inject)

```sh
./unload -n cs2          # dlclose twice (unloader's ref + the inject run's leaked ref)
```

`unload_self` (exported by the SO) stops the data thread (waits for it),
restores the hooked bytes, destroys the uinput device and releases the X11
grabs, so the library can be dlclosed without crashing the game. After
re-injecting, recreate the swapchain once (toggle fullscreen / resize / join
a match) for the overlay to initialize.

## Config & offsets

CS2 offsets change on almost every update. Offsets and scan patterns live in
`config/cs2_config.json`:

```json
{ "patterns": [...], "required": [...], "offsets": { "dwCSGOInput": "0x4576F18", ... } }
```

`src/patterns.cpp` scans the running game for the byte patterns; named offsets
(dwCSGOInput, viewAngleOffset, bone layout, aim punch, ...) are read from the
`offsets` section. Refresh with the published linux dump:

```sh
git clone -b linux https://github.com/a2x/cs2-dumper.git
cd cs2-dumper && cargo build --release
# start CS2, then:
./target/release/cs2-dumper                 # writes ./output/*
```

then copy the relevant values into `config/cs2_config.json` (or run
`scripts/dump_local.sh` for a local dump helper).

### Offset version

Current offsets in `config/cs2_config.json` are based on the **2026-08-13**
cs2-dumper output. `data/dump/` holds the **2026-08-12** schema dumps
(`info.json` timestamp) used as a reference for the field layout; refresh both
after every game update.

## How it works

The library is a set of collaborating classes, all created inside the injected
SO. The **data thread** reads the game state and drives the aim features; the
**Vulkan present hook** renders the overlay into the swapchain image.

```
cs2 process
   │  injected libcs2_internal.so
   │
   ├─ data thread (entry.cpp)
   │    OffsetResolver.attach(pid)      → live pattern scan (libclient.so)
   │    Game game(mem)                  → pawn, view matrix/angles, CEntityList
   │        ├── Aimbot(game)            → CalcAngle(head) → FOV → visibility (BVH)
   │        ├── Triggerbot(game)        → delayed fire, flash/speed/head checks
   │        ├── Rcs(game)               → independent recoil compensation
   │        └── MouseDevice g_mouse     → /dev/uinput virtual mouse (aim + fire)
   │    check_bvh(aimbot, mem, off)     → map-change BVH reload (200 ms)
   │    Logger::instance()              → /tmp/cs2_internal.log
   │
   └─ VulkanHook g_vk_hook (present hook)
         ├─ hook64::Hook × 4           → loader stubs + real ICD functions
         ├─ on_present()                → every frame before vkQueuePresentKHR
         │     ├── ImGuiRenderer        → ImGui_ImplVulkan_NewFrame/Render
         │     └── Overlay g_overlay    → ESP boxes/bones/health + control panel
         └─ on_create_swapchain_game()  → (re)init overlay on swapchain rebuild
```

Dependency injection keeps the classes decoupled: `Game` owns the `Memory`
handle, and `Aimbot` / `Triggerbot` / `Rcs` are constructed with a `Game&`, so
no feature method takes a `Game`/`Memory` parameter. `Overlay` / `MouseDevice` /
`VulkanHook` / `ImGuiRenderer` are process-wide singletons (`g_overlay`,
`g_mouse`, `g_vk_hook`).

Key files:

| File | Purpose |
|---|---|
| `internal/entry.cpp` | constructor, data thread, unload_self |
| `internal/vk_hook.cpp` / `hook_x64.{h,cpp}` | Vulkan loader + ICD inline hooks (`hook64::Hook`) |
| `internal/imgui_renderer.cpp` | ImGui context / Vulkan backend / theme |
| `internal/overlay_draw.cpp` | ImGui ESP (box/skeleton/health) + control panel |
| `internal/mouse_device.cpp` | uinput virtual mouse + smoothed movement |
| `internal/input_x11.cpp` | game-window lookup, key grabs, raw mouse for the menu |
| `src/patterns.cpp` | pattern scanning of the live binary |
| `src/cfg.cpp` | JSON config loading |
| `src/logger.cpp` | thread-safe unified logger |
| `src/aimbot.cpp` / `src/trigger.cpp` / `src/rcs.cpp` | aim / triggerbot / recoil classes |
| `src/game.cpp` | game state reads (players, weapons, bones) |

## Compositor / desktop notes

| Desktop | Status |
|---|---|
| GNOME (Wayland) | ✅ F1, ESP, aimbot, mouse all work (recursive window lookup required; Mutter reparents X11 windows) |
| Hyprland | ⚠️ menu/ESP work in lobby; in a match with raw input the mouse may not move (investigating) |
| X11 | should work (untested recently) |

The game runs as an XWayland client (`SDL_VIDEO_DRIVER=x11`); global key
grabs and pointer tracking go through the X server, so compositor
differences matter.

## FAQ

- **Does it bypass VAC?** No. Nothing here evades anti-cheat; this project is
  for learning and private research. VAC and VAC Live detect known cheat
  signatures and memory-access patterns (injection + hooks increase the
  surface).
- **Why uinput instead of writing view angles?** Writing angles is detected
  more easily and fights the game's own prediction; a virtual mouse behaves
  like real input, supports raw-input matches, and recoil compensation just
  works.
- **Why is the aim slow / not hitting?** Tune FOV / smooth / RCS strength in
  the menu; refresh offsets after every game update.
- **The overlay is not showing.** Inject earlier (before the swapchain
  exists) or recreate the swapchain: toggle fullscreen / resize / join a
  match.
- **F1 does nothing on GNOME.** Re-inject with the current build — the
  recursive window lookup fixes Mutter's reparenting (old builds logged
  `game_win=0x0` in `/tmp/cs2_internal.log`).

## License

MIT for this project's code. Dear ImGui (vendored in `third_party/imgui`) is
MIT licensed; see its `LICENSE.txt`.
