// getMemClean_tray.c - RAMMap-style memory cleaner that lives in the system tray
//
// Compile: gcc getMemClean_tray.c -o getMemClean_tray.exe -mwindows -lgdi32
//          (-mwindows = no console window)
//
// First run:
//   1) asks "Clean every how many seconds?" (plus the modes and a RAM limit)
//      and saves the answers in getMemClean.ini (same folder as the .exe)
//   2) asks if it should start with Windows and stay in the tray
// Later: click the tray icon -> "Settings..." to change the answers.
//
// getMemClean.ini (shared with the console version, getMemClean.c):
//     [getMemClean]
//     modes=w          w = working sets, m = modified, t = standby, 0 = low-prio standby
//     interval=10      seconds between cleanings (0 = automatic cleaning off)
//     limit=0          only clean if RAM usage >= this % (0 = always)
//
// How it works:
//   - Asks for administrator by itself (UAC) if you just double-click it.
//   - Creates an INVISIBLE window, only to receive the tray icon and timer messages.
//   - "Start with Windows" creates a SCHEDULED TASK with highest privileges.
//     (the registry "Run" key does NOT work here: Windows does not start
//      administrator programs from it without a UAC prompt, so it would never
//      start by itself)

#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Part 1: the cleaning itself (same as getMemClean.c)
// ---------------------------------------------------------------------------

typedef LONG (NTAPI *NtSetSystemInformation_t)(ULONG, PVOID, ULONG);

#define SystemMemoryListInformation 80
#define MemoryEmptyWorkingSets             2
#define MemoryFlushModifiedList            3
#define MemoryPurgeStandbyList             4
#define MemoryPurgeLowPriorityStandbyList  5

static NtSetSystemInformation_t NtSet;

static BOOL enable_privilege(const char *name) {
    HANDLE token;
    TOKEN_PRIVILEGES tp;

    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return FALSE;

    if (!LookupPrivilegeValueA(NULL, name, &tp.Privileges[0].Luid)) {
        CloseHandle(token);
        return FALSE;
    }

    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL);
    DWORD err = GetLastError();   // read it right away, before any other Win32 call
    CloseHandle(token);

    return err == ERROR_SUCCESS;  // ERROR_NOT_ALL_ASSIGNED = failed
}

static void get_mem(DWORD *load, double *avail_mb) {
    MEMORYSTATUSEX m;
    m.dwLength = sizeof(m);
    GlobalMemoryStatusEx(&m);
    *load     = m.dwMemoryLoad;
    *avail_mb = (double)m.ullAvailPhys / (1024.0 * 1024.0);
}

// ---------------------------------------------------------------------------
// Part 2: program state
// ---------------------------------------------------------------------------

#define WM_TRAY        (WM_APP + 1)   // message sent by the tray icon to our window
#define ID_TIMER       1
#define ID_NOW         101
#define ID_PAUSE       102
#define ID_AUTOSTART   103
#define ID_SETTINGS    104
#define ID_EXIT        105
#define TASK_NAME      "getMemClean"
#define INI_SECTION    "getMemClean"

static char  g_exe[MAX_PATH];         // full path of the .exe
static char  g_dir[MAX_PATH];         // folder of the .exe
static char  g_ini[MAX_PATH];         // full path of getMemClean.ini
static char  g_modes[16]  = "w";
static int   g_interval   = 10;
static int   g_limit      = 0;
static BOOL  g_paused     = FALSE;
static BOOL  g_autostart  = FALSE;
static char  g_status[96] = "Starting...";
static UINT  g_wm_taskbar;            // "TaskbarCreated": Explorer broadcasts it when it restarts
static HWND  g_hwnd;
static NOTIFYICONDATAA g_nid;

// ---------------------------------------------------------------------------
// Part 3: settings file
// ---------------------------------------------------------------------------

static BOOL valid_modes(const char *s) {
    if (*s == '\0') return FALSE;
    for (; *s; s++)
        if (!strchr("wmt0", *s)) return FALSE;
    return TRUE;
}

static void load_config(void) {
    char raw[64];
    GetPrivateProfileStringA(INI_SECTION, "modes", "w", raw, sizeof(raw), g_ini);
    if (valid_modes(raw) && strlen(raw) < sizeof(g_modes))
        snprintf(g_modes, sizeof(g_modes), "%s", raw);
    else
        snprintf(g_modes, sizeof(g_modes), "w");

    g_interval = (int)GetPrivateProfileIntA(INI_SECTION, "interval", 10, g_ini);
    g_limit    = (int)GetPrivateProfileIntA(INI_SECTION, "limit",    0,  g_ini);
    if (g_interval < 0) g_interval = 0;
    if (g_interval > 86400) g_interval = 86400;
    if (g_limit < 0)   g_limit = 0;
    if (g_limit > 100) g_limit = 100;
}

static BOOL save_config(void) {
    char num[16];
    BOOL ok = WritePrivateProfileStringA(INI_SECTION, "modes", g_modes, g_ini);

    snprintf(num, sizeof(num), "%d", g_interval);
    ok = ok && WritePrivateProfileStringA(INI_SECTION, "interval", num, g_ini);

    snprintf(num, sizeof(num), "%d", g_limit);
    ok = ok && WritePrivateProfileStringA(INI_SECTION, "limit", num, g_ini);

    return ok;
}

// ---------------------------------------------------------------------------
// Part 4: start with Windows (scheduled task)
// ---------------------------------------------------------------------------

// Runs a command hidden (no window), waits, and returns its exit code
static DWORD run_hidden(const char *cmdline) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char buf[2048];
    DWORD code = (DWORD)-1;

    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    snprintf(buf, sizeof(buf), "%s", cmdline);   // CreateProcess may change the buffer

    if (CreateProcessA(NULL, buf, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                       NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 30000);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return code;   // 0 = success
}

static BOOL autostart_exists(void) {
    return run_hidden("schtasks /query /tn " TASK_NAME) == 0;
}

static BOOL autostart_enable(void) {
    // Task: at logon, run this .exe with highest privileges (no UAC prompt),
    // with no time limit (the Windows default would kill it after 3 days),
    // also when running on battery.
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \""
        "$ErrorActionPreference='Stop'; "
        "$a = New-ScheduledTaskAction -Execute '%s' -WorkingDirectory '%s'; "
        "$t = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME; "
        "$p = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Highest; "
        "$s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries "
        "-ExecutionTimeLimit (New-TimeSpan -Seconds 0); "
        "Register-ScheduledTask -TaskName '%s' -Action $a -Trigger $t -Principal $p -Settings $s -Force | Out-Null"
        "\"",
        g_exe, g_dir, TASK_NAME);
    return run_hidden(cmd) == 0;
}

static BOOL autostart_disable(void) {
    return run_hidden("schtasks /delete /tn " TASK_NAME " /f") == 0;
}

// ---------------------------------------------------------------------------
// Part 5: the settings window ("Clean every how many seconds?")
// ---------------------------------------------------------------------------

#define ID_ED_INTERVAL 201
#define ID_ED_LIMIT    202
#define ID_CK_BASE     210            // 210..213 = the four mode checkboxes

static const char  MODE_CHARS[4]     = { 'w', 'm', 't', '0' };
static const char *MODE_LABELS[4]    = { "Working sets (w)", "Modified list (m)",
                                         "Standby list (t)", "Low-priority standby (0)" };

static HWND s_dlg, s_ed_interval, s_ed_limit, s_ck[4];
static BOOL s_done, s_saved;

static HWND make_child(HWND parent, const char *cls, const char *text,
                       DWORD style, DWORD exstyle,
                       int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExA(exstyle, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, parent, (HMENU)(INT_PTR)id,
                             GetModuleHandleA(NULL), NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return c;
}

// Reads a whole number between 0 and max from an edit box
static BOOL read_int_field(HWND ed, int max, int *out) {
    char buf[32];
    GetWindowTextA(ed, buf, sizeof(buf));

    char *end;
    long v = strtol(buf, &end, 10);
    while (*end == ' ') end++;
    if (end == buf || *end != '\0' || v < 0 || v > max)
        return FALSE;

    *out = (int)v;
    return TRUE;
}

static LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            char num[16];

            make_child(hwnd, "STATIC",
                       "Clean every how many seconds?  (0 = off, use \"Clean now\")",
                       0, 0, 16, 16, 308, 18, -1);
            snprintf(num, sizeof(num), "%d", g_interval);
            s_ed_interval = make_child(hwnd, "EDIT", num,
                                       ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP,
                                       WS_EX_CLIENTEDGE, 16, 38, 80, 24, ID_ED_INTERVAL);

            make_child(hwnd, "STATIC", "Modes:", 0, 0, 16, 78, 308, 18, -1);
            for (int i = 0; i < 4; i++) {
                int x = 16 + (i % 2) * 160;
                int y = 98 + (i / 2) * 24;
                s_ck[i] = make_child(hwnd, "BUTTON", MODE_LABELS[i],
                                     BS_AUTOCHECKBOX | WS_TABSTOP, 0,
                                     x, y, 156, 20, ID_CK_BASE + i);
                if (strchr(g_modes, MODE_CHARS[i]))
                    SendMessageA(s_ck[i], BM_SETCHECK, BST_CHECKED, 0);
            }

            make_child(hwnd, "STATIC",
                       "Only clean if RAM usage is at least (%)  (0 = always)",
                       0, 0, 16, 156, 308, 18, -1);
            snprintf(num, sizeof(num), "%d", g_limit);
            s_ed_limit = make_child(hwnd, "EDIT", num,
                                    ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP,
                                    WS_EX_CLIENTEDGE, 16, 178, 80, 24, ID_ED_LIMIT);

            make_child(hwnd, "BUTTON", "Save",   BS_DEFPUSHBUTTON | WS_TABSTOP, 0,
                       160, 214, 76, 26, IDOK);
            make_child(hwnd, "BUTTON", "Cancel", BS_PUSHBUTTON | WS_TABSTOP, 0,
                       244, 214, 76, 26, IDCANCEL);
            return 0;
        }

        case WM_COMMAND:
            if (LOWORD(wp) == IDOK) {
                int interval, limit, n = 0;
                char modes[8];

                if (!read_int_field(s_ed_interval, 86400, &interval)) {
                    MessageBoxA(hwnd, "Please type a number of seconds from 0 to 86400.",
                                "getMemClean", MB_ICONWARNING);
                    SetFocus(s_ed_interval);
                    return 0;
                }
                if (!read_int_field(s_ed_limit, 100, &limit)) {
                    MessageBoxA(hwnd, "Please type a RAM usage from 0 to 100.",
                                "getMemClean", MB_ICONWARNING);
                    SetFocus(s_ed_limit);
                    return 0;
                }
                for (int i = 0; i < 4; i++)
                    if (SendMessageA(s_ck[i], BM_GETCHECK, 0, 0) == BST_CHECKED)
                        modes[n++] = MODE_CHARS[i];
                modes[n] = '\0';
                if (n == 0) {
                    MessageBoxA(hwnd, "Pick at least one mode.",
                                "getMemClean", MB_ICONWARNING);
                    return 0;
                }

                g_interval = interval;
                g_limit    = limit;
                snprintf(g_modes, sizeof(g_modes), "%s", modes);
                if (!save_config())
                    MessageBoxA(hwnd, "Could not save getMemClean.ini (is the folder read-only?).\n"
                                      "The new settings work until you close the program.",
                                "getMemClean", MB_ICONWARNING);

                s_saved = TRUE;
                s_done  = TRUE;
                DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wp) == IDCANCEL) {
                s_done = TRUE;
                DestroyWindow(hwnd);
                return 0;
            }
            break;

        case WM_CLOSE:
            s_done = TRUE;
            DestroyWindow(hwnd);
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

// Shows the settings window and waits until it is closed.
// Returns TRUE if the user pressed Save.
static BOOL show_settings(const char *title) {
    static BOOL registered = FALSE;
    HINSTANCE hi = GetModuleHandleA(NULL);

    // already open (clicked the menu twice): just bring it to the front
    if (s_dlg && IsWindow(s_dlg)) {
        SetForegroundWindow(s_dlg);
        return FALSE;
    }

    if (!registered) {
        WNDCLASSA wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc   = SettingsProc;
        wc.hInstance     = hi;
        wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = "GetMemCleanSettings";
        RegisterClassA(&wc);
        registered = TRUE;
    }

    // outer window size that gives us a 340x254 client area
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    DWORD ex    = WS_EX_DLGMODALFRAME | WS_EX_TOPMOST;
    RECT r = { 0, 0, 340, 254 };
    AdjustWindowRectEx(&r, style, FALSE, ex);
    int w = r.right - r.left;
    int h = r.bottom - r.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    s_done  = FALSE;
    s_saved = FALSE;
    s_dlg = CreateWindowExA(ex, "GetMemCleanSettings", title, style,
                            x, y, w, h, NULL, NULL, hi, NULL);
    if (!s_dlg) return FALSE;

    ShowWindow(s_dlg, SW_SHOW);
    SetForegroundWindow(s_dlg);
    SetFocus(s_ed_interval);
    SendMessageA(s_ed_interval, EM_SETSEL, 0, -1);   // select the number, so typing replaces it

    // Our own little message loop, until the window closes.
    // IsDialogMessage gives us Tab, Enter (= Save) and Esc (= Cancel).
    MSG m;
    while (!s_done) {
        BOOL got = GetMessageA(&m, NULL, 0, 0);
        if (got <= 0) {                  // WM_QUIT arrived (user clicked Exit)
            PostQuitMessage((int)m.wParam);   // pass it on, so the main loop also ends
            break;
        }
        if (!IsDialogMessageA(s_dlg, &m)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
    }

    if (IsWindow(s_dlg)) DestroyWindow(s_dlg);
    s_dlg = NULL;
    return s_saved;
}

// ---------------------------------------------------------------------------
// Part 6: tray icon
// ---------------------------------------------------------------------------

// Text shown when the mouse hovers over the icon
static void update_tip(void) {
    snprintf(g_nid.szTip, sizeof(g_nid.szTip), "getMemClean%s - %s",
             g_paused ? " (paused)" : "", g_status);
    Shell_NotifyIconA(NIM_MODIFY, &g_nid);
}

static void set_idle_status(void) {
    if (g_interval > 0)
        snprintf(g_status, sizeof(g_status), "every %ds, modes %s", g_interval, g_modes);
    else
        snprintf(g_status, sizeof(g_status), "automatic cleaning off");
}

static void add_tray_icon(void) {
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = g_hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon            = LoadIcon(NULL, IDI_APPLICATION);   // default Windows icon
    snprintf(g_nid.szTip, sizeof(g_nid.szTip), "getMemClean - %s", g_status);
    Shell_NotifyIconA(NIM_ADD, &g_nid);
}

static void show_menu(void) {
    POINT pt;
    GetCursorPos(&pt);

    HMENU m = CreatePopupMenu();
    AppendMenuA(m, MF_STRING, ID_NOW, "Clean now");
    AppendMenuA(m, MF_STRING | (g_paused ? MF_CHECKED : 0), ID_PAUSE, "Pause automatic cleaning");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING, ID_SETTINGS, "Settings...");
    AppendMenuA(m, MF_STRING | (g_autostart ? MF_CHECKED : 0), ID_AUTOSTART, "Start with Windows");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING, ID_EXIT, "Exit");

    SetForegroundWindow(g_hwnd);   // without this the menu does not close when you click outside
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hwnd, NULL);
    PostMessageA(g_hwnd, WM_NULL, 0, 0);   // pair of SetForegroundWindow (Microsoft's recipe)
    DestroyMenu(m);
}

// ---------------------------------------------------------------------------
// Part 7: one cleaning cycle
// ---------------------------------------------------------------------------

static void do_clean(BOOL forced) {
    DWORD load_before, load_after;
    double avail_before, avail_after;

    get_mem(&load_before, &avail_before);

    if (!forced && (int)load_before < g_limit) {
        snprintf(g_status, sizeof(g_status), "usage %lu%% < limit %d%%",
                 (unsigned long)load_before, g_limit);
        update_tip();
        return;
    }

    LONG failed = 0;
    for (const char *p = g_modes; *p; p++) {
        int cmd = 0;
        switch (*p) {
            case 'w': cmd = MemoryEmptyWorkingSets;            break;
            case 'm': cmd = MemoryFlushModifiedList;           break;
            case 't': cmd = MemoryPurgeStandbyList;            break;
            case '0': cmd = MemoryPurgeLowPriorityStandbyList; break;
        }
        LONG status = NtSet(SystemMemoryListInformation, &cmd, sizeof(cmd));
        if (status != 0) failed = status;
    }

    Sleep(300);   // let Windows settle the numbers
    get_mem(&load_after, &avail_after);

    if (failed)
        snprintf(g_status, sizeof(g_status), "error 0x%08lX", (unsigned long)failed);
    else
        snprintf(g_status, sizeof(g_status), "available: %.0f -> %.0f MB",
                 avail_before, avail_after);
    update_tip();
}

static void restart_timer(void) {
    KillTimer(g_hwnd, ID_TIMER);
    if (g_interval > 0)
        SetTimer(g_hwnd, ID_TIMER, (UINT)g_interval * 1000, NULL);
}

// ---------------------------------------------------------------------------
// Part 8: the invisible window (receives all the messages)
// ---------------------------------------------------------------------------

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Explorer restarted and wiped the tray icons: put ours back
    if (msg == g_wm_taskbar) {
        Shell_NotifyIconA(NIM_ADD, &g_nid);
        return 0;
    }

    switch (msg) {
        case WM_TIMER:
            if (!g_paused) do_clean(FALSE);
            return 0;

        case WM_TRAY:   // click on the tray icon
            if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP)
                show_menu();
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_NOW:
                    do_clean(TRUE);
                    break;

                case ID_PAUSE:
                    g_paused = !g_paused;
                    update_tip();
                    break;

                case ID_SETTINGS:
                    if (show_settings("getMemClean settings")) {
                        restart_timer();
                        set_idle_status();
                        update_tip();
                    }
                    break;

                case ID_AUTOSTART:
                    if (g_autostart) {
                        if (autostart_disable()) g_autostart = FALSE;
                        else MessageBoxA(hwnd, "Could not remove the scheduled task.",
                                         "getMemClean", MB_ICONERROR);
                    } else {
                        if (autostart_enable()) g_autostart = TRUE;
                        else MessageBoxA(hwnd, "Could not create the scheduled task.",
                                         "getMemClean", MB_ICONERROR);
                    }
                    break;

                case ID_EXIT:
                    DestroyWindow(hwnd);
                    break;
            }
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, ID_TIMER);
            Shell_NotifyIconA(NIM_DELETE, &g_nid);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Part 9: WinMain (no console, so no plain main)
// ---------------------------------------------------------------------------

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdline, int show) {
    (void)hPrev; (void)show;

    // Paths: .exe, its folder, and getMemClean.ini
    GetModuleFileNameA(NULL, g_exe, MAX_PATH);
    snprintf(g_dir, sizeof(g_dir), "%s", g_exe);
    char *slash = strrchr(g_dir, '\\');
    if (slash) *slash = '\0';
    snprintf(g_ini, sizeof(g_ini), "%s\\getMemClean.ini", g_dir);

    // 1) Must be administrator. If not, ask for UAC and reopen elevated.
    //    The --elevated argument prevents reopening in a loop if something goes wrong.
    if (!enable_privilege("SeProfileSingleProcessPrivilege")) {
        if (strstr(cmdline, "--elevated")) {
            MessageBoxA(NULL, "Could not get the required privilege. "
                              "Your account must be an administrator.",
                        "getMemClean", MB_ICONERROR);
            return 1;
        }
        HINSTANCE r = ShellExecuteA(NULL, "runas", g_exe, "--elevated", g_dir, SW_SHOWNORMAL);
        if ((INT_PTR)r <= 32)
            MessageBoxA(NULL, "getMemClean needs administrator rights to work.",
                        "getMemClean", MB_ICONWARNING);
        return 0;
    }

    // 2) Only one copy at a time (autostart + opening it by hand won't duplicate)
    HANDLE mutex = CreateMutexA(NULL, FALSE, "Local\\getMemClean_single_instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;

    // 3) Resolve the undocumented NT kernel function from ntdll
    NtSet = (NtSetSystemInformation_t)GetProcAddress(
                GetModuleHandleA("ntdll.dll"), "NtSetSystemInformation");
    if (NtSet == NULL) {
        MessageBoxA(NULL, "Could not find NtSetSystemInformation in ntdll.",
                    "getMemClean", MB_ICONERROR);
        return 1;
    }

    // 4) Settings. If getMemClean.ini does not exist yet, this is the first run.
    BOOL first = (GetFileAttributesA(g_ini) == INVALID_FILE_ATTRIBUTES);
    load_config();                  // defaults if there is no file
    g_autostart = autostart_exists();

    if (first) {
        save_config();              // so it only asks once, even if you press Cancel
        show_settings("getMemClean - first run");   // "how many seconds?"

        if (!g_autostart) {
            int r = MessageBoxA(NULL,
                "Do you want getMemClean to start with Windows and stay in the tray?\n\n"
                "You can change this later in the tray icon menu.",
                "getMemClean", MB_YESNO | MB_ICONQUESTION);
            if (r == IDYES) {
                if (autostart_enable()) g_autostart = TRUE;
                else MessageBoxA(NULL, "Could not create the scheduled task.",
                                 "getMemClean", MB_ICONERROR);
            }
        }
    }

    // 5) Invisible window (we never call ShowWindow on it)
    g_wm_taskbar = RegisterWindowMessageA("TaskbarCreated");

    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = "GetMemCleanWindow";
    RegisterClassA(&wc);

    g_hwnd = CreateWindowExA(0, "GetMemCleanWindow", "getMemClean", 0,
                             0, 0, 0, 0, NULL, NULL, hInst, NULL);
    if (!g_hwnd) return 1;

    // 6) Icon + timer
    set_idle_status();
    add_tray_icon();
    restart_timer();

    // 7) Message loop: the program stays here until you click "Exit"
    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }

    CloseHandle(mutex);
    return 0;
}
