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
| ESP: box + health + skeleton | ImGui drawn into the game's Vulkan swapchain (`internal/vk_hook.cpp` + `internal/overlay_draw.cpp`) |
| Aimbot (FOV-limited, smoothed, inertia) | uinput virtual mouse (TI-84 disguise), deadlocked-style recoil compensation, auto-fire on lock |
| Triggerbot | optional, same uinput device |
| Offset resolution | Osiris-style pattern scans on the live binary + JSON config (`config/cs2_config.json`) |
| Unload | remote `unload_self` + `injector`-based unloader tool (restores hooks, releases X11 grabs) |

## Build

Requirements: a C++20 compiler, CMake ≥ 3.16, Vulkan and X11 dev packages
(ImGui is vendored in `third_party/imgui`, MIT license).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Produces `build/libcs2_internal.so` plus selftests
(`./build/vk_hook_selftest`, `./build/hook64_test`, `./build/scan_test`).

## Inject & run

```sh
# your favourite SO injector, e.g. the kubo/injector-based one in this repo:
./injector -n cs2 libcs2_internal.so
```

On load the library:

1. spawns a data thread that pattern-scans the live binary and resolves
   offsets (paths/config in `config/cs2_config.json`, `CS2_CONFIG` env
   overrides the path),
2. inline-hooks `vkCreateInstance` / `vkCreateDevice` /
   `vkCreateSwapchainKHR` / `vkQueuePresentKHR` in `libvulkan.so.1`,
3. initializes the ImGui overlay on the next swapchain (re)creation,
4. opens an X11 connection to find the game window (`_NET_WM_PID`, recursive
   tree walk — required on GNOME/Mutter which reparents X11 windows),
5. lazily creates the uinput virtual mouse on first aim (kept alive).

Controls:

- `F1` (or `p`) — toggle the menu panel
- hold `x` — aimbot (key grabbed globally via XGrabKey)
- menu toggles ESP / aimbot / triggerbot, FOV, smooth, etc.

Progress/errors go to `/tmp/cs2_internal.log`. X11 grab diagnostics go to
`/tmp/cs2_x11_diag.log`.

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

## How it works

```
cs2 process
   │  injected libcs2_internal.so
   ├── data thread ──► patterns::resolve (live pattern scan)
   │                    Memory reads (process_vm_readv, no ptrace attach)
   │                    Game: local pawn, view matrix/angles, CEntityList
   │                    bones: pawn → CGameSceneNode → CSkeletonInstance
   │                    aimbot: CalcAngle(head) → FOV → inertia → uinput move
   └── vk_hook ──► detour_present → ImGui (ESP + panel) into swapchain
```

Key files:

| File | Purpose |
|---|---|
| `internal/entry.cpp` | constructor, data thread, unload_self |
| `internal/vk_hook.cpp` / `hook_x64.h` | Vulkan loader inline hooks + trampolines |
| `internal/input_x11.cpp` | game-window lookup, key grabs, raw mouse for the menu |
| `internal/uinput_aim.cpp` | virtual mouse creation + smoothed movement |
| `internal/overlay_draw.cpp` | ImGui ESP (box/skeleton) + control panel |
| `src/patterns.cpp` | pattern scanning of the live binary |
| `src/cfg.cpp` | JSON config loading (loaded once) |
| `src/aimbot.cpp` / `src/trigger.cpp` | aim/recoil logic, triggerbot |
| `scripts/check_layout.sh` | layout-stability check vs saved baseline |

## Layout sensitivity (read before editing)

This cheat is **sensitive to .text layout shifts**. A verified experiment:
adding one never-called dead function shifted every function address and
reproduced a "mouse breaks after inject" bug; reverting it fixed the mouse.
Root cause is still under investigation — treat ANY layout shift as a release
blocker:

```sh
scripts/check_layout.sh --save   # after verifying a good build
scripts/check_layout.sh          # after each build; fails if layout moved
```

Known triggers:

- `constexpr float k = 1.0f` lets the compiler fold `x*1.0` away, shrinking
  `move_to()` and shifting every subsequent function — keep
  `kFracPerFrame = 0.5f` (or move the constant to another TU with external
  linkage and verify with the script).
- Any added/removed function changes addresses; run the script after changes.

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
- **Why is the aim slow / not hitting?** `kFracPerFrame` halves the movement
  per frame by design (a full-speed 1.0 build is what shifted the layout and
  broke the mouse). Tune FOV / smooth / inertia in the menu; refresh offsets
  after every game update.
- **The overlay is not showing.** Inject earlier (before the swapchain
  exists) or recreate the swapchain: toggle fullscreen / resize / join a
  match.
- **F1 does nothing on GNOME.** Re-inject with the current build — the
  recursive window lookup fixes Mutter's reparenting (old builds logged
  `game_win=0x0` in `/tmp/cs2_x11_diag.log`).

## License

MIT for this project's code. Dear ImGui (vendored in `third_party/imgui`) is
MIT licensed; see its `LICENSE.txt`.
