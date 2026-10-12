# getMemClean

**A lightweight, native Windows memory utility written in C.**

getMemClean is a Windows system-tray application that asks the Windows NT memory manager to trim working sets. It uses native Windows APIs rather than a browser runtime or a large application framework.

The intended everyday setup is **`w` mode only**. Other memory-list operations are available, but they do different things and are not required for the main use case.

> [!IMPORTANT]
> Memory cleaning is not a universal performance tweak. It does not create RAM, reduce an application's memory allocations, or guarantee higher FPS. Test the effect on your own system and stop using a setting if it causes stutter or worse performance.

## Features

- **Working-set trimming (`w`)** — the primary operation; can be run manually or on a timer.
- **Optional memory-list operations** — modified-page and standby-list commands are available for advanced use (`m`, `t`, `0`).
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
