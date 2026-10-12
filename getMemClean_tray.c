// getMemClean_tray.c - Modern System Tray RAM Cleaning Utility
// 
// Compilation:
//   gcc getMemClean_tray.c -o getMemClean_tray.exe -mwindows -lgdi32 -lcomctl32
//   (-mwindows: hides the console window; -lcomctl32: links Windows Common Controls)

#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Part 1: Native Memory Cleaning Engine
// ---------------------------------------------------------------------------

// Function pointer signature for the undocumented Native API function in ntdll.dll
typedef LONG (NTAPI *NtSetSystemInformation_t)(ULONG, PVOID, ULONG);

// Constants corresponding to Windows memory management operations
#define SystemMemoryListInformation 80
#define MemoryEmptyWorkingSets             2
#define MemoryFlushModifiedList            3
#define MemoryPurgeStandbyList             4
#define MemoryPurgeLowPriorityStandbyList  5

static NtSetSystemInformation_t NtSet;

/**
 * Enables a specific privilege for the current process token.
 * Required to invoke low-level NT system functions.
 */
static BOOL enable_privilege(const char *name) {
    HANDLE token;
    TOKEN_PRIVILEGES tp;

    // Open process token with privileges adjustment rights
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return FALSE;

    // Retrieve the Locally Unique Identifier (LUID) for the privilege name
    if (!LookupPrivilegeValueA(NULL, name, &tp.Privileges[0].Luid)) {
        CloseHandle(token);
        return FALSE;
    }

    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    // Apply the privilege modification
    AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL);
    DWORD err = GetLastError(); // Fetch error code immediately
    CloseHandle(token);

    return err == ERROR_SUCCESS; // Returns FALSE if ERROR_NOT_ALL_ASSIGNED
}

/**
 * Retrieves current RAM usage percentage and available physical memory in MB.
 */
static void get_mem(DWORD *load, double *avail_mb) {
    MEMORYSTATUSEX m;
    m.dwLength = sizeof(m);
    GlobalMemoryStatusEx(&m);
    *load     = m.dwMemoryLoad;
    *avail_mb = (double)m.ullAvailPhys / (1024.0 * 1024.0);
}

// ---------------------------------------------------------------------------
// Part 2: Program State & Global Variables
// ---------------------------------------------------------------------------

#define WM_TRAY        (WM_APP + 1)  // Custom window message for system tray events
#define ID_TIMER       1             // Periodic cleaning timer ID
#define ID_SETTINGS_TIMER 2          // Settings dialog live update timer ID
#define ID_NOW         101
#define ID_PAUSE       102
#define ID_AUTOSTART   103
#define ID_SETTINGS    104
#define ID_EXIT        105
#define TASK_NAME      "getMemClean"
#define INI_SECTION    "getMemClean"

static char  g_exe[MAX_PATH];         // Executable path
static char  g_dir[MAX_PATH];         // Application directory
static char  g_ini[MAX_PATH];         // Path to getMemClean.ini
static char  g_modes[16]  = "w";       // Configured cleaning modes
static int   g_interval   = 10;        // Interval in seconds
static int   g_limit      = 0;         // Minimum RAM threshold (%)
static BOOL  g_paused     = FALSE;     // Pause toggle
static BOOL  g_autostart  = FALSE;     // Autostart scheduled task status
static char  g_status[96] = "Starting...";
static UINT  g_wm_taskbar;            // "TaskbarCreated" message handle
static HWND  g_hwnd;                  // Background window handle
static NOTIFYICONDATAA g_nid;         // Tray icon notification data

// ---------------------------------------------------------------------------
// Part 3: INI File Configuration Manager
// ---------------------------------------------------------------------------

/**
 * Validates whether the given string contains only valid mode characters ('w', 'm', 't', '0').
 */
static BOOL valid_modes(const char *s) {
    if (*s == '\0') return FALSE;
    for (; *s; s++)
        if (!strchr("wmt0", *s)) return FALSE;
    return TRUE;
}

/**
 * Loads user settings from getMemClean.ini. Applies safe fallback defaults.
 */
static void load_config(void) {
    char raw[64];
    GetPrivateProfileStringA(INI_SECTION, "modes", "w", raw, sizeof(raw), g_ini);
    if (valid_modes(raw) && strlen(raw) < sizeof(g_modes))
        snprintf(g_modes, sizeof(g_modes), "%s", raw);
    else
        snprintf(g_modes, sizeof(g_modes), "w");

    g_interval = (int)GetPrivateProfileIntA(INI_SECTION, "interval", 10, g_ini);
    g_limit    = (int)GetPrivateProfileIntA(INI_SECTION, "limit",    0,  g_ini);
    
    // Clamp configuration boundaries
    if (g_interval < 0) g_interval = 0;
    if (g_interval > 86400) g_interval = 86400;
    if (g_limit < 0)   g_limit = 0;
    if (g_limit > 100) g_limit = 100;
}

/**
 * Saves current runtime settings to getMemClean.ini.
 */
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
// Part 4: Windows Autostart via Task Scheduler
// ---------------------------------------------------------------------------

/**
 * Executes a process silently without showing a console window and awaits termination.
 */
static DWORD run_hidden(const char *cmdline) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char buf[2048];
    DWORD code = (DWORD)-1;

    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    snprintf(buf, sizeof(buf), "%s", cmdline);

    if (CreateProcessA(NULL, buf, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 30000); // 30-second execution timeout
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return code; // 0 indicates successful command execution
}

/**
 * Checks if the scheduled task already exists.
 */
static BOOL autostart_exists(void) {
    return run_hidden("schtasks /query /tn " TASK_NAME) == 0;
}

/**
 * Creates an elevated Scheduled Task at user logon using PowerShell.
 * (Bypasses Windows UAC limitations associated with traditional registry 'Run' keys).
 */
static BOOL autostart_enable(void) {
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

/**
 * Removes the scheduled autostart task.
 */
static BOOL autostart_disable(void) {
    return run_hidden("schtasks /delete /tn " TASK_NAME " /f") == 0;
}

// ---------------------------------------------------------------------------
// Part 5: Settings GUI Window & Live Stats Panel
// ---------------------------------------------------------------------------

#define ID_ED_INTERVAL 201
#define ID_ED_LIMIT    202
#define ID_CK_BASE     210
#define ID_LBL_STATS   220

static const char  MODE_CHARS[4]  = { 'w', 'm', 't', '0' };
static const char *MODE_LABELS[4] = { 
    "Working sets (w)", 
    "Modified list (m)",
    "Standby list (t)", 
    "Low-priority standby (0)" 
};

static HWND s_dlg, s_ed_interval, s_ed_limit, s_ck[4], s_lbl_stats;
static BOOL s_done, s_saved;

/**
 * Helper function to create child controls with standard modern font applied.
 */
static HWND make_child(HWND parent, const char *cls, const char *text, DWORD style, DWORD exstyle, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExA(exstyle, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent, (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return c;
}

/**
 * Parses integers from text fields and performs range validation.
 */
static BOOL read_int_field(HWND ed, int max, int *out) {
    char buf[32];
    GetWindowTextA(ed, buf, sizeof(buf));
    char *end;
    long v = strtol(buf, &end, 10);
    while (*end == ' ') end++;
    if (end == buf || *end != '\0' || v < 0 || v > max) return FALSE;
    *out = (int)v;
    return TRUE;
}

/**
 * Updates memory usage info in the live display header of the settings dialog.
 */
static void update_dialog_stats(void) {
    if (!s_lbl_stats || !IsWindow(s_lbl_stats)) return;
    DWORD load;
    double avail;
    get_mem(&load, &avail);
    char buf[128];
    snprintf(buf, sizeof(buf), "RAM Usage: %lu%%  |  Available: %.1f GB", (unsigned long)load, avail / 1024.0);
    SetWindowTextA(s_lbl_stats, buf);
}

/**
 * Window procedure for the Settings dialog window.
 */
static LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            char num[16];

            // Live RAM stats panel
            s_lbl_stats = make_child(hwnd, "STATIC", "Loading memory stats...", SS_CENTERIMAGE, WS_EX_CLIENTEDGE, 16, 16, 388, 30, ID_LBL_STATS);
            update_dialog_stats();
            SetTimer(hwnd, ID_SETTINGS_TIMER, 1000, NULL); // Refresh stats every second

            make_child(hwnd, "STATIC", "Clean interval in seconds (0 = manual cleaning only):", 0, 0, 16, 62, 388, 18, -1);
            snprintf(num, sizeof(num), "%d", g_interval);
            s_ed_interval = make_child(hwnd, "EDIT", num, ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 16, 82, 100, 24, ID_ED_INTERVAL);

            make_child(hwnd, "STATIC", "Memory Cleaning Modes:", 0, 0, 16, 120, 388, 18, -1);
            for (int i = 0; i < 4; i++) {
                int y = 142 + (i * 24);
                s_ck[i] = make_child(hwnd, "BUTTON", MODE_LABELS[i], BS_AUTOCHECKBOX | WS_TABSTOP, 0, 16, y, 388, 20, ID_CK_BASE + i);
                if (strchr(g_modes, MODE_CHARS[i]))
                    SendMessageA(s_ck[i], BM_SETCHECK, BST_CHECKED, 0);
            }

            make_child(hwnd, "STATIC", "Only clean if RAM usage is at least (%) (0 = always):", 0, 0, 16, 246, 388, 18, -1);
            snprintf(num, sizeof(num), "%d", g_limit);
            s_ed_limit = make_child(hwnd, "EDIT", num, ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 16, 268, 100, 24, ID_ED_LIMIT);

            // Action buttons
            make_child(hwnd, "BUTTON", "Save", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, 228, 312, 84, 28, IDOK);
            make_child(hwnd, "BUTTON", "Cancel", BS_PUSHBUTTON | WS_TABSTOP, 0, 320, 312, 84, 28, IDCANCEL);
            return 0;
        }

        case WM_TIMER:
            if (wp == ID_SETTINGS_TIMER) {
                update_dialog_stats();
            }
            return 0;

        case WM_COMMAND:
            if (LOWORD(wp) == IDOK) {
                int interval, limit, n = 0;
                char modes[8];

                if (!read_int_field(s_ed_interval, 86400, &interval)) {
                    MessageBoxA(hwnd, "Please enter a valid interval between 0 and 86400 seconds.", "getMemClean", MB_ICONWARNING);
                    SetFocus(s_ed_interval);
                    return 0;
                }
                if (!read_int_field(s_ed_limit, 100, &limit)) {
                    MessageBoxA(hwnd, "Please enter a valid memory limit percentage (0 to 100).", "getMemClean", MB_ICONWARNING);
                    SetFocus(s_ed_limit);
                    return 0;
                }
                for (int i = 0; i < 4; i++)
                    if (SendMessageA(s_ck[i], BM_GETCHECK, 0, 0) == BST_CHECKED)
                        modes[n++] = MODE_CHARS[i];
                modes[n] = '\0';
                if (n == 0) {
                    MessageBoxA(hwnd, "Please select at least one cleaning mode.", "getMemClean", MB_ICONWARNING);
                    return 0;
                }

                g_interval = interval;
                g_limit    = limit;
                snprintf(g_modes, sizeof(g_modes), "%s", modes);
                if (!save_config())
                    MessageBoxA(hwnd, "Could not save settings to getMemClean.ini.", "getMemClean", MB_ICONWARNING);

                s_saved = TRUE;
                s_done  = TRUE;
                KillTimer(hwnd, ID_SETTINGS_TIMER);
                DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wp) == IDCANCEL) {
                s_done = TRUE;
                KillTimer(hwnd, ID_SETTINGS_TIMER);
                DestroyWindow(hwnd);
                return 0;
            }
            break;

        case WM_CLOSE:
            s_done = TRUE;
            KillTimer(hwnd, ID_SETTINGS_TIMER);
            DestroyWindow(hwnd);
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/**
 * Creates and shows the Settings dialog modal window.
 */
static BOOL show_settings(const char *title) {
    static BOOL registered = FALSE;
    HINSTANCE hi = GetModuleHandleA(NULL);

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
        wc.lpszClassName = "GetMemCleanSettingsModern";
        RegisterClassA(&wc);
        registered = TRUE;
    }

    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    DWORD ex    = WS_EX_DLGMODALFRAME | WS_EX_TOPMOST;
    RECT r = { 0, 0, 420, 360 };
    AdjustWindowRectEx(&r, style, FALSE, ex);
    int w = r.right - r.left;
    int h = r.bottom - r.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    s_done  = FALSE;
    s_saved = FALSE;
    s_dlg = CreateWindowExA(ex, "GetMemCleanSettingsModern", title, style, x, y, w, h, NULL, NULL, hi, NULL);
    if (!s_dlg) return FALSE;

    ShowWindow(s_dlg, SW_SHOW);
    SetForegroundWindow(s_dlg);
    SetFocus(s_ed_interval);
    SendMessageA(s_ed_interval, EM_SETSEL, 0, -1);

    // Dialog modal message loop with keyboard accelerator support (Tab, Enter, Esc)
    MSG m;
    while (!s_done) {
        BOOL got = GetMessageA(&m, NULL, 0, 0);
        if (got <= 0) {
            PostQuitMessage((int)m.wParam);
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
// Part 6: System Tray Icon & Notifications
// ---------------------------------------------------------------------------

/**
 * Updates the tray icon hover tooltip text.
 */
static void update_tip(void) {
    snprintf(g_nid.szTip, sizeof(g_nid.szTip), "getMemClean%s\n%s", g_paused ? " [Paused]" : "", g_status);
    Shell_NotifyIconA(NIM_MODIFY, &g_nid);
}

/**
 * Displays a popup balloon notification from the tray icon.
 */
static void show_notification(const char *title, const char *msg) {
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_INFO;
    snprintf(g_nid.szInfoTitle, sizeof(g_nid.szInfoTitle), "%s", title);
    snprintf(g_nid.szInfo, sizeof(g_nid.szInfo), "%s", msg);
    g_nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconA(NIM_MODIFY, &g_nid);
}

/**
 * Resets status text based on current schedule settings.
 */
static void set_idle_status(void) {
    if (g_interval > 0)
        snprintf(g_status, sizeof(g_status), "Every %ds | Modes: %s", g_interval, g_modes);
    else
        snprintf(g_status, sizeof(g_status), "Automatic cleaning disabled");
}

/**
 * Registers the application icon in the Windows notification tray.
 */
static void add_tray_icon(void) {
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = g_hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon            = LoadIcon(NULL, IDI_APPLICATION);
    snprintf(g_nid.szTip, sizeof(g_nid.szTip), "getMemClean - %s", g_status);
    Shell_NotifyIconA(NIM_ADD, &g_nid);
}

/**
 * Displays the right-click context menu at the mouse cursor position.
 */
static void show_menu(void) {
    POINT pt;
    GetCursorPos(&pt);

    HMENU m = CreatePopupMenu();
    AppendMenuA(m, MF_STRING, ID_NOW, "Clean memory now");
    AppendMenuA(m, MF_STRING | (g_paused ? MF_CHECKED : 0), ID_PAUSE, "Pause auto-cleaning");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING, ID_SETTINGS, "Settings...");
    AppendMenuA(m, MF_STRING | (g_autostart ? MF_CHECKED : 0), ID_AUTOSTART, "Start with Windows");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING, ID_EXIT, "Exit");

    // Required workaround to properly handle popup menu dismissal when clicking outside
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hwnd, NULL);
    PostMessageA(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

// ---------------------------------------------------------------------------
// Part 7: Memory Cleaning Cycle Execution
// ---------------------------------------------------------------------------

/**
 * Performs a memory cleaning routine.
 * @param forced - If TRUE, ignores RAM threshold limit checks.
 */
static void do_clean(BOOL forced) {
    DWORD load_before, load_after;
    double avail_before, avail_after;

    get_mem(&load_before, &avail_before);

    // Skip auto-cleaning cycle if RAM load is below threshold
    if (!forced && (int)load_before < g_limit) {
        snprintf(g_status, sizeof(g_status), "Load %lu%% < Limit %d%%", (unsigned long)load_before, g_limit);
        update_tip();
        return;
    }

    LONG failed = 0;
    // Iterate through configured mode characters ('w', 'm', 't', '0')
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

    Sleep(300); // Allow Windows kernel memory statistics to settle
    get_mem(&load_after, &avail_after);

    if (failed) {
        snprintf(g_status, sizeof(g_status), "Error 0x%08lX", (unsigned long)failed);
    } else {
        snprintf(g_status, sizeof(g_status), "Available: %.0f -> %.0f MB", avail_before, avail_after);
        if (forced) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Memory cleaned successfully!\nBefore: %.0f MB | After: %.0f MB", avail_before, avail_after);
            show_notification("getMemClean - Manual Clean", msg);
        }
    }
    update_tip();
}

/**
 * Resets or updates the background interval timer.
 */
static void restart_timer(void) {
    KillTimer(g_hwnd, ID_TIMER);
    if (g_interval > 0)
        SetTimer(g_hwnd, ID_TIMER, (UINT)g_interval * 1000, NULL);
}

// ---------------------------------------------------------------------------
// Part 8: Invisible Message-Handling Window Procedure
// ---------------------------------------------------------------------------

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Restore system tray icon if Windows Explorer restarts
    if (msg == g_wm_taskbar) {
        Shell_NotifyIconA(NIM_ADD, &g_nid);
        return 0;
    }

    switch (msg) {
        case WM_TIMER:
            if (!g_paused) do_clean(FALSE);
            return 0;

        case WM_TRAY:
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
                    if (show_settings("getMemClean - Settings")) {
                        restart_timer();
                        set_idle_status();
                        update_tip();
                    }
                    break;

                case ID_AUTOSTART:
                    if (g_autostart) {
                        if (autostart_disable()) g_autostart = FALSE;
                        else MessageBoxA(hwnd, "Failed to delete scheduled task.", "getMemClean", MB_ICONERROR);
                    } else {
                        if (autostart_enable()) g_autostart = TRUE;
                        else MessageBoxA(hwnd, "Failed to create scheduled task.", "getMemClean", MB_ICONERROR);
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
// Part 9: Application Entry Point (WinMain)
// ---------------------------------------------------------------------------

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdline, int show) {
    (void)hPrev; (void)show;

    // Enable Windows visual styles (ComCtl32 v6)
    INITCOMMONCONTROLSEX icce;
    icce.dwSize = sizeof(icce);
    icce.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icce);

    // Resolve current executable path and working directory
    GetModuleFileNameA(NULL, g_exe, MAX_PATH);
    snprintf(g_dir, sizeof(g_dir), "%s", g_exe);
    char *slash = strrchr(g_dir, '\\');
    if (slash) *slash = '\0';
    snprintf(g_ini, sizeof(g_ini), "%s\\getMemClean.ini", g_dir);

    // Ensure elevated administrative privileges via UAC prompt if needed
    if (!enable_privilege("SeProfileSingleProcessPrivilege")) {
        if (strstr(cmdline, "--elevated")) {
            MessageBoxA(NULL, "Insufficient privileges. Your account must be an Administrator.", "getMemClean", MB_ICONERROR);
            return 1;
        }
        HINSTANCE r = ShellExecuteA(NULL, "runas", g_exe, "--elevated", g_dir, SW_SHOWNORMAL);
        if ((INT_PTR)r <= 32)
            MessageBoxA(NULL, "getMemClean requires administrator rights to function.", "getMemClean", MB_ICONWARNING);
        return 0;
    }

    // Single-instance enforcement using a named system mutex
    HANDLE mutex = CreateMutexA(NULL, FALSE, "Local\\getMemClean_single_instance_modern");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    // Load NtSetSystemInformation dynamically from ntdll.dll
    NtSet = (NtSetSystemInformation_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtSetSystemInformation");
    if (NtSet == NULL) {
        MessageBoxA(NULL, "Could not locate NtSetSystemInformation in ntdll.dll.", "getMemClean", MB_ICONERROR);
        return 1;
    }

    BOOL first = (GetFileAttributesA(g_ini) == INVALID_FILE_ATTRIBUTES);
    load_config();
    g_autostart = autostart_exists();

    // First-time configuration setup
    if (first) {
        save_config();
        show_settings("getMemClean - First Run Setup");

        if (!g_autostart) {
            int r = MessageBoxA(NULL,
                "Would you like getMemClean to start automatically with Windows?\n\n"
                "You can change this anytime from the system tray menu.",
                "getMemClean", MB_YESNO | MB_ICONQUESTION);
            if (r == IDYES) {
                if (autostart_enable()) g_autostart = TRUE;
                else MessageBoxA(NULL, "Could not enable autostart task.", "getMemClean", MB_ICONERROR);
            }
        }
    }

    // Register TaskbarCreated message to handle Explorer shell restarts
    g_wm_taskbar = RegisterWindowMessageA("TaskbarCreated");

    // Register background host window class
    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = "GetMemCleanWindowModern";
    RegisterClassA(&wc);

    // Create hidden message-receiver window
    g_hwnd = CreateWindowExA(0, "GetMemCleanWindowModern", "getMemClean", 0, 0, 0, 0, 0, NULL, NULL, hInst, NULL);
    if (!g_hwnd) return 1;

    set_idle_status();
    add_tray_icon();
    restart_timer();

    // Main Win32 application message loop
    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }

    CloseHandle(mutex);
    return 0;
}