#ifndef WATCHDOG_RULES_H
#define WATCHDOG_RULES_H

#include <stdint.h>
#include <stddef.h>

/* No Win32 dependency: the sampling and alert policy can be tested directly. */
typedef struct {
    uint32_t pid;
    uint64_t created;
    uint64_t first_private;
    uint64_t previous_private;
    uint64_t last_private;
    unsigned increases;
    unsigned samples;
    unsigned alerted; /* bit 0: high usage, bit 1: sustained growth */
} WatchState;

typedef struct {
    int high;
    int growth;
    int notify;
    uint64_t growth_bytes;
} WatchDecision;

#define WATCH_MB (UINT64_C(1024) * 1024)

static WatchDecision watch_sample(WatchState *s, uint32_t pid, uint64_t created,
                                  uint64_t private_bytes, uint64_t limit_mb) {
    WatchDecision d = {0, 0, 0, 0};
    uint64_t limit = limit_mb > UINT64_MAX / WATCH_MB ? UINT64_MAX : limit_mb * WATCH_MB;
    if (s->pid != pid || s->created != created) {
        *s = (WatchState){0};
        s->pid = pid;
        s->created = created;
        s->first_private = private_bytes;
    } else {
        /* Small fluctuations do not count as sustained growth. */
        if (s->previous_private <= UINT64_MAX - 4 * WATCH_MB &&
            private_bytes >= s->previous_private + 4 * WATCH_MB)
            s->increases++;
        else
            s->increases = 0;
    }
    s->samples++;
    s->last_private = private_bytes;
    s->previous_private = private_bytes;
    d.high = limit_mb != 0 && private_bytes >= limit;
    d.growth = d.high && s->increases >= 3 && private_bytes >= s->first_private &&
               private_bytes - s->first_private >= 16 * WATCH_MB;
    d.growth_bytes = private_bytes >= s->first_private ? private_bytes - s->first_private : 0;
    if (!d.high && private_bytes < limit - limit / 10)
        s->alerted = 0;
    if (d.high && !(s->alerted & 1)) {
        d.notify = 1;
        s->alerted |= 1;
    } else if (d.growth && !(s->alerted & 2)) {
        d.notify = 1;
        s->alerted |= 2;
    }
    return d;
}

static int watch_valid_name(const char *name) {
    size_t n = 0;
    if (!name || !*name) return 0;
    while (name[n]) {
        unsigned char c = (unsigned char)name[n++];
        if (n > 259 || c < 32 || c == '=' || c == '[' || c == ']' || c == '\\' || c == '/') return 0;
    }
    return n >= 5 && name[n - 4] == '.' &&
           (name[n - 3] == 'e' || name[n - 3] == 'E') &&
           (name[n - 2] == 'x' || name[n - 2] == 'X') &&
           (name[n - 1] == 'e' || name[n - 1] == 'E');
}

#endif
