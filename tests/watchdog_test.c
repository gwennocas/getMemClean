#include "../watchdog.h"
#include "../watchdog_rules.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void detection_tests(void) {
    WatchState s = {0};
    WatchDecision d;
    d = watch_sample(&s, 100, 1000, 100 * WATCH_MB, 128);
    assert(!d.high && !d.notify && !d.growth);
    d = watch_sample(&s, 100, 1000, 130 * WATCH_MB, 128);
    assert(d.high && d.notify && !d.growth);
    d = watch_sample(&s, 100, 1000, 138 * WATCH_MB, 128);
    assert(d.high && !d.notify && !d.growth);
    d = watch_sample(&s, 100, 1000, 146 * WATCH_MB, 128);
    assert(d.high && d.growth && d.notify && d.growth_bytes == 46 * WATCH_MB);
    d = watch_sample(&s, 100, 1000, 154 * WATCH_MB, 128);
    assert(d.growth && !d.notify);
    d = watch_sample(&s, 100, 1000, 100 * WATCH_MB, 128);
    assert(!d.high && !d.notify);
    d = watch_sample(&s, 100, 1000, 130 * WATCH_MB, 128);
    assert(d.high && d.notify);
    d = watch_sample(&s, 100, 2000, 140 * WATCH_MB, 128);
    assert(d.high && d.notify && !d.growth && s.created == 2000);
    d = watch_sample(&s, 100, 2000, 142 * WATCH_MB, 128);
    assert(d.high && !d.growth && !d.notify);
}

static void config_tests(void) {
    WatchConfig cfg = {0}, loaded;
    char text[512];
    char path[MAX_PATH];
    assert(GetFullPathNameA("watchdog_test.ini", sizeof(path), path, NULL));
    cfg.enabled = 1;
    cfg.interval_sec = 5;
    cfg.global_mb = 1024;
    assert(watch_valid_name("Editor.exe"));
    assert(!watch_valid_name("..\\Editor.exe"));
    assert(watch_parse_rules("Editor.exe=512\r\nBrowser.exe=2048\r\n", &cfg));
    assert(watch_limit_for(&cfg, "EDITOR.EXE") == 512);
    assert(watch_limit_for(&cfg, "other.exe") == 1024);
    assert(!watch_parse_rules("app.exe=0", &cfg));
    assert(!watch_parse_rules("app.exe=1\r\nAPP.EXE=2", &cfg));
    assert(!watch_parse_rules("../app.exe=100", &cfg));
    assert(cfg.count == 2);
    assert(watch_parse_ignored("editor.exe\r\n", &cfg));
    assert(watch_limit_for(&cfg, "Editor.exe") == 0);
    watch_format_rules(&cfg, text, sizeof(text));
    assert(strstr(text, "Browser.exe=2048") != NULL);
    watch_format_ignored(&cfg, text, sizeof(text));
    assert(strstr(text, "editor.exe") != NULL);
    watch_init(NULL, path);
    watch_set_config(&cfg);
    assert(watch_save_config());
    watch_stop();
    watch_init(NULL, path);
    watch_get_config(&loaded);
    assert(loaded.enabled && loaded.interval_sec == 5 && loaded.global_mb == 1024);
    assert(watch_limit_for(&loaded, "EDITOR.EXE") == 0);
    assert(watch_limit_for(&loaded, "Browser.exe") == 2048);
    watch_stop();
    loaded.count = 0;
    loaded.ignored_count = 0;
    watch_init(NULL, path);
    watch_set_config(&loaded);
    assert(watch_save_config());
    watch_stop();
    watch_init(NULL, path);
    watch_get_config(&loaded);
    assert(loaded.count == 0 && loaded.ignored_count == 0);
    watch_stop();
    DeleteFileA(path);
}

static void notification_and_error_tests(void) {
    WatchAlert a = {0};
    char text[512];
    a.pid = GetCurrentProcessId();
    a.created = 1;
    strcpy(a.name, "example.exe");
    a.private_bytes = 150 * WATCH_MB;
    a.working_set = 75 * WATCH_MB;
    a.growth_bytes = 25 * WATCH_MB;
    a.limit_mb = 128;
    watch_format_alert(&a, text, sizeof(text));
    assert(strstr(text, "example.exe") && strstr(text, "PID") && strstr(text, "150 MB"));
    assert(strstr(text, "75 MB") && strstr(text, "128 MB"));
    assert(strstr(text, "insufficient evidence"));
    a.growth = TRUE;
    watch_format_alert(&a, text, sizeof(text));
    assert(strstr(text, "+25 MB") && strstr(text, "possible leak signal"));
    /* The application and stale identities must never be operated on. */
    assert(watch_trim(&a) != ERROR_SUCCESS);
    assert(watch_end(&a) != ERROR_SUCCESS);
    a.pid = 0;
    assert(watch_trim(&a) != ERROR_SUCCESS);
}

int main(void) {
    detection_tests();
    config_tests();
    notification_and_error_tests();
    puts("watchdog tests passed");
    return 0;
}
