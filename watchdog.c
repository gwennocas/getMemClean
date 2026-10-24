#define _WIN32_WINNT 0x0601
#include "watchdog.h"
#include "watchdog_rules.h"
#include <tlhelp32.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define WATCH_MAX_TRACKED 4096
typedef struct { WatchState state; unsigned generation; } Tracked;

static HWND s_owner;
static char s_ini[MAX_PATH];
static WatchConfig s_cfg;
static CRITICAL_SECTION s_lock;
static HANDLE s_stop, s_thread;
static volatile LONG s_outstanding;

static uint64_t filetime_number(FILETIME t) {
    return ((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime;
}

static int same_name(const char *a, const char *b) { return _stricmp(a, b) == 0; }

static BOOL process_identity(HANDLE h, uint64_t *created) {
    FILETIME c, e, k, u;
    if (!GetProcessTimes(h, &c, &e, &k, &u)) return FALSE;
    *created = filetime_number(c);
    return TRUE;
}

/* IsProcessCritical is available on newer Windows. Older systems use a
   conservative list of core system images. Failed classification fails closed. */
static BOOL safe_target(HANDLE h, DWORD pid) {
    typedef BOOL (WINAPI *CriticalFn)(HANDLE, PBOOL);
    FARPROC proc = GetProcAddress(GetModuleHandleA("kernel32.dll"), "IsProcessCritical");
    CriticalFn critical;
    memcpy(&critical, &proc, sizeof(critical));
    BOOL yes = FALSE;
    if (pid <= 4 || pid == GetCurrentProcessId()) return FALSE;
    if (critical) return critical(h, &yes) && !yes;
    {
        char path[MAX_PATH];
        DWORD n = sizeof(path);
        const char *name;
        static const char *blocked[] = {"smss.exe", "csrss.exe", "wininit.exe", "services.exe", "lsass.exe", "winlogon.exe", "svchost.exe", "System", "Registry"};
        unsigned i;
        if (!QueryFullProcessImageNameA(h, 0, path, &n)) return FALSE;
        name = strrchr(path, '\\');
        name = name ? name + 1 : path;
        for (i = 0; i < sizeof(blocked) / sizeof(blocked[0]); i++)
            if (same_name(name, blocked[i])) return FALSE;
    }
    return TRUE;
}

unsigned watch_limit_for(const WatchConfig *cfg, const char *name) {
    unsigned i;
    for (i = 0; i < cfg->ignored_count; i++) if (same_name(cfg->ignored[i], name)) return 0;
    for (i = 0; i < cfg->count; i++) if (same_name(cfg->rules[i].name, name)) return cfg->rules[i].mb;
    return cfg->global_mb;
}

static Tracked *slot_for(Tracked *items, DWORD pid, uint64_t created, unsigned generation) {
    unsigned i;
    Tracked *free_slot = NULL;
    for (i = 0; i < WATCH_MAX_TRACKED; i++) {
        if (items[i].state.pid == pid && items[i].state.created == created) return &items[i];
        if (!free_slot && (!items[i].state.pid || items[i].generation + 1 < generation)) free_slot = &items[i];
    }
    if (free_slot) ZeroMemory(free_slot, sizeof(*free_slot));
    return free_slot;
}

static void scan(Tracked *items, unsigned generation, const WatchConfig *cfg) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 entry;
    unsigned posted = 0;
    if (snapshot == INVALID_HANDLE_VALUE) return;
    entry.dwSize = sizeof(entry);
    if (Process32First(snapshot, &entry)) do {
        HANDLE h;
        uint64_t created;
        PROCESS_MEMORY_COUNTERS_EX mem;
        Tracked *slot;
        WatchDecision d;
        WatchAlert *alert;
        unsigned limit = watch_limit_for(cfg, entry.szExeFile);
        if (strlen(entry.szExeFile) >= sizeof(((WatchAlert *)0)->name)) continue;
        if (!limit || entry.th32ProcessID <= 4 || entry.th32ProcessID == GetCurrentProcessId()) continue;
        h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_QUERY_INFORMATION, FALSE, entry.th32ProcessID);
        if (!h) continue;
        if (!process_identity(h, &created) || !safe_target(h, entry.th32ProcessID)) { CloseHandle(h); continue; }
        ZeroMemory(&mem, sizeof(mem));
        mem.cb = sizeof(mem);
        if (!GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS *)&mem, sizeof(mem))) { CloseHandle(h); continue; }
        CloseHandle(h);
        slot = slot_for(items, entry.th32ProcessID, created, generation);
        if (!slot) continue;
        slot->generation = generation;
        d = watch_sample(&slot->state, entry.th32ProcessID, created, (uint64_t)mem.PrivateUsage, limit);
        if (!d.notify) continue;
        if (posted >= 8 || InterlockedCompareExchange(&s_outstanding, 0, 0) >= 8) {
            slot->state.alerted &= ~(d.growth ? 2u : 1u);
            continue;
        }
        alert = (WatchAlert *)calloc(1, sizeof(*alert));
        if (!alert) continue;
        alert->pid = entry.th32ProcessID;
        alert->created = created;
        memcpy(alert->name, entry.szExeFile, strlen(entry.szExeFile) + 1);
        alert->private_bytes = (uint64_t)mem.PrivateUsage;
        alert->working_set = (uint64_t)mem.WorkingSetSize;
        alert->growth_bytes = d.growth_bytes;
        alert->limit_mb = limit;
        alert->growth = d.growth;
        InterlockedIncrement(&s_outstanding);
        if (!PostMessageA(s_owner, WM_WATCH_ALERT, 0, (LPARAM)alert)) {
            free(alert);
            InterlockedDecrement(&s_outstanding);
            slot->state.alerted &= ~(d.growth ? 2u : 1u);
        } else posted++;
    } while (Process32Next(snapshot, &entry) && WaitForSingleObject(s_stop, 0) != WAIT_OBJECT_0);
    CloseHandle(snapshot);
}

static DWORD WINAPI worker(LPVOID unused) {
    Tracked *items = (Tracked *)calloc(WATCH_MAX_TRACKED, sizeof(*items));
    unsigned generation = 0;
    (void)unused;
    if (!items) return 1;
    while (WaitForSingleObject(s_stop, 0) != WAIT_OBJECT_0) {
        WatchConfig cfg;
        DWORD delay;
        watch_get_config(&cfg);
        if (cfg.enabled) scan(items, ++generation, &cfg);
        delay = (cfg.interval_sec < 5 ? 5 : cfg.interval_sec) * 1000;
        if (WaitForSingleObject(s_stop, delay) == WAIT_OBJECT_0) break;
    }
    free(items);
    return 0;
}

static unsigned read_number(const char *section, const char *key, unsigned fallback, unsigned max) {
    UINT v = GetPrivateProfileIntA(section, key, fallback, s_ini);
    return v > max ? fallback : v;
}

static void read_rules(WatchConfig *cfg) {
    char buffer[32768], *p;
    buffer[0] = 0;
    GetPrivateProfileSectionA("WatchdogLimits", buffer, sizeof(buffer), s_ini);
    for (p = buffer; *p && cfg->count < WATCH_MAX_RULES; p += strlen(p) + 1) {
        char *eq = strchr(p, '=');
        char *end;
        unsigned long mb;
        if (!eq) continue;
        *eq++ = 0;
        mb = strtoul(eq, &end, 10);
        if (!watch_valid_name(p) || *end || !mb || mb > 1048576) continue;
        memcpy(cfg->rules[cfg->count].name, p, strlen(p) + 1);
        cfg->rules[cfg->count++].mb = (unsigned)mb;
    }
    buffer[0] = 0;
    GetPrivateProfileSectionA("WatchdogIgnored", buffer, sizeof(buffer), s_ini);
    for (p = buffer; *p && cfg->ignored_count < WATCH_MAX_RULES; p += strlen(p) + 1) {
        char *eq = strchr(p, '=');
        if (eq) *eq = 0;
        if (watch_valid_name(p)) {
            memcpy(cfg->ignored[cfg->ignored_count++], p, strlen(p) + 1);
        }
    }
}

void watch_init(HWND hwnd, const char *ini) {
    s_owner = hwnd;
    snprintf(s_ini, sizeof(s_ini), "%s", ini);
    InitializeCriticalSection(&s_lock);
    ZeroMemory(&s_cfg, sizeof(s_cfg));
    s_cfg.enabled = read_number("MemoryWatchdog", "enabled", 0, 1);
    s_cfg.interval_sec = read_number("MemoryWatchdog", "interval", 15, 3600);
    if (s_cfg.interval_sec < 5) s_cfg.interval_sec = 15;
    s_cfg.global_mb = read_number("MemoryWatchdog", "global_mb", 2048, 1048576);
    read_rules(&s_cfg);
    s_stop = CreateEventA(NULL, TRUE, FALSE, NULL);
}

void watch_get_config(WatchConfig *out) {
    EnterCriticalSection(&s_lock);
    *out = s_cfg;
    LeaveCriticalSection(&s_lock);
}

void watch_set_config(const WatchConfig *cfg) {
    EnterCriticalSection(&s_lock);
    s_cfg = *cfg;
    LeaveCriticalSection(&s_lock);
}

BOOL watch_save_config(void) {
    WatchConfig cfg;
    char number[32];
    unsigned i;
    BOOL ok;
    watch_get_config(&cfg);
    ok = WritePrivateProfileStringA("MemoryWatchdog", "enabled", cfg.enabled ? "1" : "0", s_ini);
    snprintf(number, sizeof(number), "%u", cfg.interval_sec);
    ok = WritePrivateProfileStringA("MemoryWatchdog", "interval", number, s_ini) && ok;
    snprintf(number, sizeof(number), "%u", cfg.global_mb);
    ok = WritePrivateProfileStringA("MemoryWatchdog", "global_mb", number, s_ini) && ok;
    WritePrivateProfileStringA("WatchdogLimits", NULL, NULL, s_ini);
    for (i = 0; i < cfg.count; i++) {
        snprintf(number, sizeof(number), "%u", cfg.rules[i].mb);
        ok = WritePrivateProfileStringA("WatchdogLimits", cfg.rules[i].name, number, s_ini) && ok;
    }
    WritePrivateProfileStringA("WatchdogIgnored", NULL, NULL, s_ini);
    for (i = 0; i < cfg.ignored_count; i++)
        ok = WritePrivateProfileStringA("WatchdogIgnored", cfg.ignored[i], "1", s_ini) && ok;
    return ok;
}

BOOL watch_ignore_forever(const char *name) {
    unsigned i;
    BOOL added = FALSE;
    EnterCriticalSection(&s_lock);
    for (i = 0; i < s_cfg.ignored_count; i++)
        if (same_name(s_cfg.ignored[i], name)) { added = TRUE; break; }
    if (!added && s_cfg.ignored_count < WATCH_MAX_RULES && watch_valid_name(name)) {
        memcpy(s_cfg.ignored[s_cfg.ignored_count++], name, strlen(name) + 1);
        added = TRUE;
    }
    LeaveCriticalSection(&s_lock);
    return added && watch_save_config();
}

BOOL watch_parse_rules(const char *text, WatchConfig *cfg) {
    WatchConfig next = *cfg;
    next.count = 0;
    while (*text) {
        char line[300], name[WATCH_NAME_SIZE], *eq, *end;
        size_t n = 0;
        unsigned long mb;
        unsigned i;
        while (text[n] && text[n] != '\r' && text[n] != '\n') n++;
        if (n >= sizeof(line)) return FALSE;
        memcpy(line, text, n); line[n] = 0;
        text += n;
        while (*text == '\r' || *text == '\n') text++;
        if (!n) continue;
        eq = strchr(line, '=');
        if (!eq) return FALSE;
        *eq++ = 0;
        if (!watch_valid_name(line) || strlen(line) >= sizeof(name)) return FALSE;
        mb = strtoul(eq, &end, 10);
        if (*end || !mb || mb > 1048576 || next.count >= WATCH_MAX_RULES) return FALSE;
        for (i = 0; i < next.count; i++) if (same_name(next.rules[i].name, line)) return FALSE;
        memcpy(next.rules[next.count].name, line, strlen(line) + 1);
        next.rules[next.count++].mb = (unsigned)mb;
    }
    *cfg = next;
    return TRUE;
}

void watch_format_rules(const WatchConfig *cfg, char *out, size_t cap) {
    unsigned i;
    size_t used = 0;
    if (!cap) return;
    out[0] = 0;
    for (i = 0; i < cfg->count && used < cap; i++) {
        int n = snprintf(out + used, cap - used, "%s=%u\r\n", cfg->rules[i].name, cfg->rules[i].mb);
        if (n < 0 || (size_t)n >= cap - used) break;
        used += (size_t)n;
    }
}

BOOL watch_parse_ignored(const char *text, WatchConfig *cfg) {
    WatchConfig next = *cfg;
    next.ignored_count = 0;
    while (*text) {
        char name[WATCH_NAME_SIZE];
        size_t n = 0;
        unsigned i;
        while (text[n] && text[n] != '\r' && text[n] != '\n') n++;
        if (n >= sizeof(name)) return FALSE;
        memcpy(name, text, n); name[n] = 0;
        text += n;
        while (*text == '\r' || *text == '\n') text++;
        if (!n) continue;
        if (!watch_valid_name(name) || next.ignored_count >= WATCH_MAX_RULES) return FALSE;
        for (i = 0; i < next.ignored_count; i++) if (same_name(name, next.ignored[i])) return FALSE;
        memcpy(next.ignored[next.ignored_count++], name, strlen(name) + 1);
    }
    *cfg = next;
    return TRUE;
}

void watch_format_ignored(const WatchConfig *cfg, char *out, size_t cap) {
    unsigned i;
    size_t used = 0;
    if (!cap) return;
    out[0] = 0;
    for (i = 0; i < cfg->ignored_count && used < cap; i++) {
        int n = snprintf(out + used, cap - used, "%s\r\n", cfg->ignored[i]);
        if (n < 0 || (size_t)n >= cap - used) break;
        used += (size_t)n;
    }
}

void watch_format_alert(const WatchAlert *a, char *out, size_t cap) {
    if (!cap) return;
    if (a->growth)
        snprintf(out, cap,
            "%s | PID %lu\r\nPrivate Bytes: %.0f MB | Working Set: %.0f MB\r\nLimit: %u MB\r\nGrowth: +%.0f MB; possible leak signal, not proof",
            a->name, (unsigned long)a->pid, (double)a->private_bytes / 1048576.0,
            (double)a->working_set / 1048576.0, a->limit_mb,
            (double)a->growth_bytes / 1048576.0);
    else
        snprintf(out, cap,
            "%s | PID %lu\r\nPrivate Bytes: %.0f MB | Working Set: %.0f MB\r\nLimit: %u MB\r\nHigh usage; insufficient evidence of a leak",
            a->name, (unsigned long)a->pid, (double)a->private_bytes / 1048576.0,
            (double)a->working_set / 1048576.0, a->limit_mb);
}

BOOL watch_start(HWND owner) {
    if (!s_stop) return FALSE;
    s_owner = owner;
    s_thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    return s_thread != NULL;
}

void watch_stop(void) {
    if (s_stop) SetEvent(s_stop);
    if (s_thread) { WaitForSingleObject(s_thread, INFINITE); CloseHandle(s_thread); s_thread = NULL; }
    if (s_stop) { CloseHandle(s_stop); s_stop = NULL; }
    DeleteCriticalSection(&s_lock);
}

void watch_alert_done(void) { InterlockedDecrement(&s_outstanding); }

static DWORD act_on_process(const WatchAlert *alert, BOOL end) {
    DWORD rights = PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_QUERY_INFORMATION |
                   (end ? PROCESS_TERMINATE : PROCESS_SET_QUOTA);
    HANDLE h = OpenProcess(rights, FALSE, alert->pid);
    uint64_t created;
    DWORD error = ERROR_ACCESS_DENIED;
    if (!h) return GetLastError();
    if (!process_identity(h, &created) || created != alert->created || !safe_target(h, alert->pid)) {
        error = ERROR_INVALID_HANDLE;
    } else if (end ? TerminateProcess(h, 1) : EmptyWorkingSet(h)) {
        error = ERROR_SUCCESS;
    } else {
        error = GetLastError();
    }
    CloseHandle(h);
    return error;
}

DWORD watch_trim(const WatchAlert *alert) { return act_on_process(alert, FALSE); }
DWORD watch_end(const WatchAlert *alert) { return act_on_process(alert, TRUE); }
