# getMemClean

A small C/Win32 system-tray utility for Windows. The existing memory-list cleaning and the optional **Memory Watchdog** operate independently.

## Features

- Manual or scheduled memory cleaning using the existing `w`, `m`, `t`, and `0` modes.
- An optional RAM-percentage condition for scheduled cleaning; pause from the tray menu.
- Memory Watchdog scans processes on a worker thread with Toolhelp and Psapi. It samples Private Bytes and Working Set without pausing the interface.
- Global Private Bytes limit and optional executable-name overrides. An override takes precedence over the global limit.
- A bottom-right alert shows the executable name, PID, Private Bytes, Working Set, configured limit, and growth trend. It offers **Trim RAM**, **End process**, and **Ignore**. The Ignore action can permanently exclude the executable name.
- Before either process action, the Watchdog checks the PID and creation time again. It skips processes Windows marks critical, the utility itself, and inaccessible processes. On older Windows versions without `IsProcessCritical`, it uses a conservative core-process exclusion list.

The Watchdog is **off by default**. Enable it in Settings. The tray's **Pause auto-cleaning** option affects only scheduled cleaning.

## What an alert means

The configured limit applies to **Private Bytes**, not Working Set. Crossing it raises a *high usage* alert. This alone is not evidence of a memory leak. The Watchdog labels *sustained growth* only after three consecutive samples each increase by at least 4 MB and the total increase since tracking began is at least 16 MB. Even then, it is only a signal for investigation, not a diagnosis.

Each process instance gets one high-usage alert and, if observed, one later growth alert. It becomes eligible again after falling below 90% of its limit. PID plus process creation time keeps a new instance from inheriting an old instance's history. Up to eight alerts per scan are queued for display; the rest remain eligible on the next scan.

**Trim RAM** calls the documented `EmptyWorkingSet` API. It can temporarily shrink the Working Set; memory may return and the action does not repair a leak. **End process** always asks for confirmation and warns about unsaved work. The Watchdog never ends processes automatically. Windows access controls may prevent either action.

## Settings

Settings are stored in `getMemClean.ini` beside the executable. The graphical Settings window manages the cleaning and Watchdog options, including per-process limits and permanent exclusions. Executable names are matched without regard to letter case.

```ini
[getMemClean]
modes=w
interval=10
limit=0

[MemoryWatchdog]
enabled=1
interval=15
global_mb=2048

[WatchdogLimits]
editor.exe=1024
browser.exe=4096

[WatchdogIgnored]
example.exe=1
```

`getMemClean.interval=0` disables scheduled cleaning, while the Watchdog keeps its own timer. `getMemClean.limit=0` disables the cleaning RAM-percentage condition. `MemoryWatchdog.global_mb=0` disables the global process limit, leaving only named limits. The scan interval is 5–3600 seconds. Limits are 1–1048576 MB. An ignored executable name overrides every limit.

## Build and tests

Use MinGW-w64 GCC on Windows:

```powershell
gcc -std=c11 -O2 getMemClean.c watchdog.c -o getMemClean.exe -mwindows -lgdi32 -lcomctl32 -lpsapi
gcc -std=c11 -Wall -Wextra -Werror -O2 tests/watchdog_test.c watchdog.c -o watchdog_test.exe -lpsapi
./watchdog_test.exe
```

If GCC is installed in `C:\msys64\ucrt64\bin`, add that directory to `PATH` before building so `cc1.exe` can find its DLLs:

```powershell
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
```

The tests cover limit overrides and exclusions, growth detection, alert suppression and rearming, PID reuse, INI persistence, alert text, and failed or stale process actions. A visual check of the notification window and tray behavior still requires an interactive Windows session.

## Notes

The existing cleaning engine uses `NtSetSystemInformation`; its effect depends on Windows and can hurt responsiveness in some workloads. Trimming a process's Working Set is also temporary. Per-process rules apply to the executable **name**, so applications with the same name share a rule. Windows may deny process queries or actions for protected processes. No heavy runtime is required beyond the Windows APIs and the MinGW runtime used to build the executable.
