#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <windows.h>
#include <stdint.h>

#define WATCH_MAX_RULES 64
#define WATCH_NAME_SIZE MAX_PATH
#define WM_WATCH_ALERT (WM_APP + 10)

typedef struct { char name[WATCH_NAME_SIZE]; unsigned mb; } WatchLimit;
typedef struct {
    BOOL enabled;
    unsigned interval_sec;
    unsigned global_mb;
    unsigned count;
    WatchLimit rules[WATCH_MAX_RULES];
    unsigned ignored_count;
    char ignored[WATCH_MAX_RULES][WATCH_NAME_SIZE];
} WatchConfig;

typedef struct {
    DWORD pid;
    uint64_t created;
    char name[WATCH_NAME_SIZE];
    uint64_t private_bytes;
    uint64_t working_set;
    uint64_t growth_bytes;
    unsigned limit_mb;
    BOOL growth;
} WatchAlert;

void watch_init(HWND hwnd, const char *ini);
void watch_get_config(WatchConfig *out);
void watch_set_config(const WatchConfig *cfg);
BOOL watch_save_config(void);
BOOL watch_ignore_forever(const char *name);
BOOL watch_parse_rules(const char *text, WatchConfig *cfg);
void watch_format_rules(const WatchConfig *cfg, char *out, size_t cap);
BOOL watch_parse_ignored(const char *text, WatchConfig *cfg);
void watch_format_ignored(const WatchConfig *cfg, char *out, size_t cap);
void watch_format_alert(const WatchAlert *alert, char *out, size_t cap);
unsigned watch_limit_for(const WatchConfig *cfg, const char *name);
BOOL watch_start(HWND owner);
void watch_stop(void);
void watch_alert_done(void);
DWORD watch_trim(const WatchAlert *alert);
DWORD watch_end(const WatchAlert *alert);

#endif
