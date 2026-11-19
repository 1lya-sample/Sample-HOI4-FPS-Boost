# Sample HOI4 FPS Boost

Speeds up Hearts of Iron IV in the late game: higher FPS at high game speeds and less load on the
game's main thread. Game files are not changed: everything happens in the memory of the running game
and is gone when the game closes.

## Launch

Double-click **`hoi4_launch.cmd`**: the game starts through Steam and the optimizations turn on by
themselves.

Or through Steam: HOI4 → Properties → Launch Options:

```
"C:\Users\Admin\Desktop\hoi4\hoi4_launch.cmd" %command%
```

Then just press "Play" in Steam as usual. Doing both is fine: the second launcher sees the first one
and does not connect to the game again.

If the game is already running without the optimizations, double-clicking `hoi4_launch.cmd` connects
them to it (it does not restart the game) and shows a window with the result. Everything starts
working right away, except space-to-pause in multiplayer: it appears after the next save is loaded.

Do not add `-render_thread` to the launch options: the hidden separate render thread mode together
with the frame pacing patch crashes the game (with it, that patch is skipped automatically).

## How it works

1. `bin\launcher.exe` (no window) starts the game through Steam (or runs Steam's command), waits for
   `hoi4.exe` and 1.5 seconds after it starts loads `bin\hoi4boost.dll` into it.
2. `hoi4boost.dll` runs inside the game and does the rest:

| What | Effect |
|---|---|
| **mimalloc** | game memory is allocated through the fast mimalloc allocator instead of the Windows heap |
| **Core isolation** | the game pins its main thread to one core; its other threads are moved off that physical core so they do not preempt the main thread |
| **Main thread priority** | "above normal" |
| `wake-all-workers` | the job scheduler wakes all worker threads at once instead of one after another |
| `frame-pacing` | at speeds 3–5 the game draws frames as often as at 1–2 (vanilla: 30 and 20 FPS while simulating) |
| `adaptive-fps` | while you use the game (mouse, keyboard, cursor at the screen edge) FPS is full; after 2 s without input or when the game is minimized, speeds 3–5 run at 30 FPS. Frames and the day simulation share one thread, so frames not drawn make the days go faster |
| `province-throttle` | visible provinces without changes refresh their map objects every second frame instead of every frame |
| `worker-spin` | idle worker threads wait ~0.1 ms for new work before going to sleep |
| `mp-space-pause` | space pauses and unpauses in multiplayer too (the game removes this key in network games, although the pause button works there) |

The engine patches find their place in the code by signatures. If a game update only moves the code,
they are found automatically. If Paradox change these functions themselves, the patch is not applied
(the game keeps working) and the log says which one and why. Game code is changed only while the
game's other threads are suspended and only if none of them is executing the bytes being replaced.

## Log

`logs\boost.log`: `[launcher]` lines come from the launcher, `[game]` lines from inside the game.
After loading a save it should contain: `hoi4boost.dll loaded`, `mimalloc ... 4 of 4`, `applied` for
all five patches, `adaptive-fps: ...` and `cpu: main thread ... pinned`.

## Settings

File `bin\hoi4boost.ini`. It is read when the boost connects to the game, so restart the game after
editing it. `1` = on, `0` = off. Everything is on by default.

### `[memory]`

| Setting | Default | What it does |
|---|---|---|
| `mimalloc` | `1` | game memory is allocated through the fast mimalloc allocator instead of the Windows heap |
| `large_pages` | `1` | large memory pages (2 MB) for mimalloc: fewer address-translation cache misses. Needs a Windows right, see below; without it normal pages are used silently |
| `reserve_huge_gib` | `0` | how many gigabytes to reserve up front in 1 GB pages (needs the same right). The memory is held for the whole game, so with 16 GB RAM use 2 at most |

The right for `large_pages` and `reserve_huge_gib`: `secpol.msc` → Local Policies → User Rights
Assignment → "Lock pages in memory" → add your account, then sign out of Windows and back in.

### `[cpu]`

| Setting | Default | What it does |
|---|---|---|
| `isolate_main_core` | `1` | the game's other threads are kept off the main thread's physical core so they do not preempt it |
| `boost_main_priority` | `1` | main thread priority "above normal" |

### `[patches]` — engine patches

| Setting | Default | What it does |
|---|---|---|
| `enabled` | `1` | all patches at once; `0` turns off the whole section |
| `wake_all_workers` | `1` | the job scheduler wakes all worker threads at once instead of one after another |
| `frame_pacing` | `1` | speeds 3–5 draw frames as often as 1–2 (vanilla: 30 and 20 FPS) |
| `target_fps` | `75` | the FPS to aim for at speeds 3–5. The engine stretches the interval when a frame is expensive, so 75 gives about 60 in practice |
| `adaptive_fps` | `1` | adaptive FPS (needs `frame_pacing`): `target_fps` while you use the game, `idle_fps` when idle |
| `idle_fps` | `30` | FPS when idle at speeds 3–5. Lower = faster days but a choppier picture, see below |
| `idle_after_ms` | `2000` | how many milliseconds without mouse and keyboard count as idle. A minimized or inactive game is idle immediately |
| `province_throttle` | `1` | visible provinces without changes refresh their map objects every second frame |
| `worker_spin` | `1` | idle worker threads briefly wait for new work before going to sleep |
| `spin_iters` | `2000` | how long to wait: iterations of the CPU `pause` instruction, 2000 ≈ 0.05–0.15 ms |

**Why FPS affects game speed.** Frames and the game-hour simulation take turns on the same main
thread, and each frame costs ~6 ms (measured, see "Measurements"). Fewer frames leave more time for
the simulation. The times in the table are calculated (6 ms × FPS), not measured:

| `idle_fps` | Time spent on frames when idle (calculated) | Smoothness |
|---|---|---|
| 60 | ~360 ms of every second | same as without adaptive FPS |
| **30** | ~180 ms | like vanilla at speeds 3–4 |
| 20 | ~120 ms | like vanilla at speed 5 |
| 10 | ~60 ms | visibly choppy |

Below 20 the gain hardly grows. Values below 10 are treated as 10. If `idle_fps` is not lower than
`target_fps`, adaptive FPS turns off. Pause and speeds 1–2 are not affected by these settings.

In multiplayer the game speed is set by the host and the slowest player, so lower FPS makes the days
faster only if that is you.

### `[controls]`

| Setting | Default | What it does |
|---|---|---|
| `mp_space_pause` | `1` | space pauses and unpauses in multiplayer too |

## Measurements

Hearts of Iron IV 1.19.5, 1939 scenario, speed 5. PC: Intel Core i5-13420H, 16 GB DDR5-5200.
Numbers from ETW CPU traces (`dev\hoi4_trace.cmd`, 15 s each) and the in-game FPS counter.

| What | Without boost | With boost | Source |
|---|---|---|---|
| FPS at speed 5 | ~30 | ~70 | in-game FPS counter |
| A typical frame arrives after | 31 ms (~32 FPS) | 15.7 ms (~64 FPS) | traces, multiplayer with the boost version without adaptive FPS |
| The slowest frames (every tenth) arrive after | — | 76 ms or more | same: these are the stutters when the game spends long on a game hour and cannot draw a frame |
| Time per frame (main thread) | ~9 ms | ~6 ms | traces; the drop comes from the game's graphics settings (trees, buildings, weather etc. off), not the boost |
| One game hour (main thread) | — | ~22–30 ms, ~80% serial code | traces |
| Main thread in multiplayer | — | computing 79% of the time, waiting for the host 16% | trace |

## Uninstall

Remove the Steam launch options (if you set them) and delete the folder. Nothing stays in the game
or in the system.

## Structure

```
hoi4_launch.cmd        launch
LICENSE                MIT License
bin\
  launcher.exe         starts the game and loads hoi4boost.dll into it
  hoi4boost.dll        optimizations inside the game
  hoi4boost.ini        settings
logs\boost.log         log (created automatically)
src\
  hoi4boost\           the DLL
    main.cpp           entry point, log, settings
    memory.cpp         mimalloc
    patcher.cpp/.h     signature search, safe code writes
    patches.cpp        the engine patches
    adaptive_fps.cpp   adaptive FPS
    cpu.cpp            core isolation and priority
  launcher\            launcher.exe
  tools\               patchtest.cpp, stress.cpp
dev\                   for development
  build.cmd            build (Visual Studio C++ and git; mimalloc is downloaded automatically)
  build\patchtest.exe  checks the patches on the real hoi4.exe without starting the game
  build\hoi4.exe       memory stress test (named so because the boost only activates in hoi4.exe)
  hoi4_trace.cmd       records a CPU trace (as administrator) to D:\hoi4_<name>.etl
  analysis\            trace and game code analysis (Python + capstone)
```

Check after building or after a game update:

```
dev\build\patchtest.exe
```

For each patch it prints whether it was found in the current `hoi4.exe`, where, and whether it was
applied and reverted successfully.

## Risks

- An antivirus may treat loading a DLL into the game as suspicious activity.