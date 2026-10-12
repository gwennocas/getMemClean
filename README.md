# getMemClean

**A lightweight, native Windows memory utility written in C.**

getMemClean is a Windows system-tray application that asks the Windows NT memory manager to trim working sets[cite: 1]. It uses native Windows APIs rather than a browser runtime or a large application framework[cite: 1].

The intended everyday setup is **`w` mode only**. Other memory-list operations are available, but they do different things and are not required for the main use case[cite: 1].

> [!IMPORTANT]
> Memory cleaning is not a universal performance tweak. It does not create RAM, reduce an application's memory allocations, or guarantee higher FPS. Test the effect on your own system and stop using a setting if it causes stutter or worse performance.

## Features

- **Working-set trimming (`w`)** — the primary operation; can be run manually or on a timer[cite: 1].
- **Optional memory-list operations** — modified-page and standby-list commands are available for advanced use (`m`, `t`, `0`)[cite: 1].
- **Automatic cleaning** — set an interval in seconds or disable the timer and clean manually[cite: 1].
- **RAM-usage threshold** — optionally run a cleaning pass only when reported memory usage reaches a chosen percentage[cite: 1].
- **System-tray interface** — open Settings, clean immediately, pause or resume automatic cleaning, and exit from the tray menu[cite: 1].
- **Persistent settings** — configuration is saved in `getMemClean.ini` beside the executable[cite: 1].
- **Native implementation** — written in C using Windows APIs, including the NT native API used for memory-list commands[cite: 1].

## Quick start

For the main use case, start with `w` mode only. In Settings, choose `w` as the mode and configure the interval to suit your system[cite: 1].

To run working-set trimming every five seconds without a RAM-usage threshold, the configuration format is:
[getMemClean]
modes=w
interval=5
limit=0

- `modes=w` enables only working-set trimming[cite: 1].
- `interval=5` requests a cleaning pass every five seconds[cite: 1].
- `limit=0` disables the RAM-usage threshold, so the interval controls automatic runs[cite: 1].

An interval of `0` disables automatic cleaning[cite: 1]. You can also trigger a cleaning pass manually from the tray menu[cite: 1].

## Memory modes

| Code | Operation | What it requests |
| --- | --- | --- |
| `w` | `MemoryEmptyWorkingSets` | Asks Windows to empty working sets. This is the primary mode[cite: 1]. |
| `m` | `MemoryFlushModifiedList` | Requests a flush of modified pages[cite: 1]. |
| `t` | `MemoryPurgeStandbyList` | Purges the standby list[cite: 1]. |
| `0` | `MemoryPurgeLowPriorityStandbyList` | Purges the low-priority portion of the standby list[cite: 1]. |

## Requirements

- Windows[cite: 1]
- MinGW-w64 GCC to build from source[cite: 1]

## Build from source

Compile the application using GCC[cite: 1]:
gcc getMemClean_tray.c -o getMemClean_tray.exe -mwindows -lgdi32

## Configuration reference

Stored in `getMemClean.ini`[cite: 1]:

| Setting | Meaning | Default |
| --- | --- | --- |
| `modes` | Memory operation codes (`w`, `m`, `t`, `0`)[cite: 1]. | `w`[cite: 1] |
| `interval` | Automatic-cleaning interval in seconds[cite: 1]. | `10`[cite: 1] |
| `limit` | Minimum RAM-usage percentage required to clean[cite: 1]. | `0`[cite: 1] |
