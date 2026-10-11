# getMemClean

**getMemClean** is a lightweight, high-performance, command-line memory management utility for Windows written in pure C. Inspired by Sysinternals RAMMap, it utilizes low-level, undocumented Windows NT kernel APIs to clean memory caches, flush modified pages, purge standby lists, and empty working sets on demand.

---

## Features

* **Granular Control:** Target specific memory types individually or combine them (`w`, `m`, `t`, `0`).
* **Continuous Monitoring:** Run it once or schedule it to run automatically at custom intervals.
* **Threshold Protection:** Set a RAM usage limit percentage so it only cleans when your system is actually under memory pressure.
* **Real-Time Feedback:** Displays clear before-and-after statistics of available RAM and usage percentages.
* **Zero Dependencies:** Written in standard C using Win32 and NT internal APIs—no external runtimes or heavy frameworks required.

---

## Compilation

To compile the source code using **GCC** (MinGW):

```bash
gcc getMemClean.c -o getMemClean.exe
