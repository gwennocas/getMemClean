# getMemClean

**A lightweight, native Windows memory utility written in C.**

getMemClean is a Windows system-tray application that asks the Windows NT memory manager to trim working sets. It uses native Windows APIs rather than a browser runtime or a large application framework.

The intended everyday setup is **`w` mode only**. Other memory-list operations are available, but they do different things and are not required for the main use case.

> [!IMPORTANT]
> Memory cleaning is not a universal performance tweak. It does not create RAM, reduce an application's memory allocations, or guarantee higher FPS. Test the effect on your own system and stop using a setting if it causes stutter or worse performance.

## Features

- **Working-set trimming (`w`)** — the primary operation; can be run manually or on a timer.
- **Optional memory-list operations** — modified-page and standby-list commands are available for advanced use.
- **Automatic cleaning** — set an interval in seconds or disable the timer and clean manually.
- **RAM-usage threshold** — optionally run a cleaning pass only when reported memory usage reaches a chosen percentage.
- **System-tray interface** — open Settings, clean immediately, pause or resume automatic cleaning, and exit from the tray menu.
- **Persistent settings** — configuration is saved in `getMemClean.ini` beside the executable.
- **Native implementation** — written in C using Windows APIs, including the NT native API used for memory-list commands.

## Quick start

For the main use case, start with `w` mode only. In Settings, choose `w` as the mode and configure the interval to suit your system.

To run working-set trimming every five seconds without a RAM-usage threshold, the configuration is:

```ini
[getMemClean]
modes=w
interval=5
limit=0
```

- `modes=w` enables only working-set trimming.
- `interval=5` requests a cleaning pass every five seconds.
- `limit=0` disables the RAM-usage threshold, so the interval—not a minimum usage percentage—controls automatic runs.

An interval of `0` disables automatic cleaning. You can also trigger a cleaning pass manually from the tray menu.

## Memory modes

| Code | Operation | What it requests |
| --- | --- | --- |
| `w` | `MemoryEmptyWorkingSets` | Asks Windows to empty working sets. This is the primary mode for getMemClean's intended use. |
| `m` | `MemoryFlushModifiedList` | Requests a flush of modified pages. This can cause disk activity and is not needed for ordinary working-set trimming. |
| `t` | `MemoryPurgeStandbyList` | Purges the standby list, removing cached pages that Windows might otherwise reuse quickly. |
| `0` | `MemoryPurgeLowPriorityStandbyList` | Purges the low-priority portion of the standby list. |

These operations are **not interchangeable**. In particular, `m`, `t`, and `0` are not required for the recommended `w`-only setup. Flushing or purging memory lists can cause Windows or applications to write, reload, or fetch data again later.

## Performance notes

In the author's own setup, enabling `w` at a five-second interval was associated with an increase from roughly **78 FPS to a stable 165 FPS**, with noticeably smoother frame pacing. This is a personal observation from one setup, not a controlled benchmark or a guarantee that other systems will see the same result. It also does not establish that working-set trimming alone caused the entire change.

Windows already manages memory automatically. Trimming working sets can make more physical memory available temporarily, but pages may need to be brought back into memory when applications use them again. Depending on the workload, that can increase page faults, reloads, or stutter instead of improving performance. Results depend on the game or application, available memory, Windows build, drivers, and other running software.

For a useful comparison:

1. Test the same game scene or workload with the same graphics settings.
2. Compare several runs with getMemClean enabled and disabled.
3. Check frame-time consistency and stutter as well as average FPS.
4. If performance worsens, increase the interval or turn automatic cleaning off.

## Requirements

- Windows
- MinGW-w64 GCC to build from source

The memory-list interface used by the project is an undocumented Windows NT native API. Its availability and behavior may vary between Windows versions, and some operations may require elevated permissions.

## Build from source

One way to build the application is with GCC from [MSYS2](https://www.msys2.org/):

1. Install MSYS2 and open its **UCRT64** terminal.
2. Install the compiler if it is not already installed:

   ```sh
   pacman -S --needed mingw-w64-ucrt-x86_64-gcc
   ```

3. From the repository directory, compile the application:

   ```sh
   gcc getMemClean.c -O2 -Wall -Wextra -mwindows -o getMemClean.exe -lgdi32 -lshell32 -ladvapi32
   ```

4. Run `getMemClean.exe` and open its system-tray menu to configure it.

Review the source and understand the requested permissions before running a binary. If Windows displays a UAC prompt, only approve elevation when you trust the executable and intended operation.

## Configuration reference

The application stores settings in `getMemClean.ini` beside `getMemClean.exe`.

| Setting | Meaning | Default |
| --- | --- | --- |
| `modes` | Memory operation codes to run. For normal use, prefer `w`. | `w` |
| `interval` | Automatic-cleaning interval in seconds. `0` disables the timer. | `10` |
| `limit` | Minimum reported RAM-usage percentage required to clean. `0` disables the threshold. | `0` |

The Settings window accepts intervals from `0` to `86400` seconds and thresholds from `0` to `100` percent. You can change these values in the app rather than editing the INI file manually.

## Reporting an issue or sharing results

Issues and reproducible test results are welcome. For performance reports, include:

- Windows version and build
- CPU, GPU, and amount of RAM
- Selected mode, interval, and threshold
- The game or workload and the scene tested
- How FPS and frame times were measured, including whether the result was repeated

## Contributing

Bug reports, documentation improvements, and code contributions are welcome. Please include enough detail for others to reproduce a problem or verify a change.

## License

There is currently no `LICENSE` file in the repository. Publicly viewable source code does not by itself grant permission to reuse, redistribute, or modify it. If you intend to accept outside contributions or allow reuse, consider choosing and adding a license that matches your goals. [Choose a License](https://choosealicense.com/) explains common options.
