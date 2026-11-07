/* 
    wctl_gui.cpp -- GUI 辅助工具
 */
#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <wchar.h>
#include "wctl_shared.h"

#define WM_APP_OUTPUT  (WM_APP + 1)
#define WM_APP_DONE    (WM_APP + 2)

#define IDC_LIST        1001
#define IDC_LOG         1002
#define IDC_CFGPATH     1003
#define IDC_BTN_RELOAD  1010
#define IDC_BTN_EDITCFG 1011
#define IDC_BTN_REFRESH 1012
#define IDC_BTN_ATTACH  1013
#define IDC_BTN_KILL    1014
#define IDC_BTN_RESTART 1015
#define IDC_BTN_DETACH  1016
#define IDC_BTN_FOLDER  1017
#define IDC_BTN_START   1018
#define IDC_CHK_FREEZE  1020
#define IDC_CHK_TOP     1021
#define IDC_CHK_FULL    1022
#define IDC_CHK_LOCK    1023
#define IDC_CHK_SIZE    1024
#define IDC_CHK_PANEL   1025
#define IDC_CHK_ALLWIN  1026
#define IDC_CHK_VERBOSE 1027
#define IDC_CMB_PANELPOS 1030
#define IDC_EDT_PANELW   1031
#define IDC_CHK_HIDEIDLE 1032
#define IDC_CHK_KEEPCT   1033
#define IDC_CHK_SCALECT  1036
#define IDC_CHK_FOCUS    1037
#define IDC_CHK_REMAP    1038
#define IDC_CHK_POSSIM   1039
#define IDC_CHK_RMSYNC   1040
#define IDC_LBL_PANELPOS 1034
#define IDC_LBL_PANELW   1035

#define MAX_ENTRIES 256

typedef struct BlackEntry {
    wchar_t name[128];    
    wchar_t path[MAX_PATH];/* 可选 */
} BlackEntry;

static BlackEntry s_black[MAX_ENTRIES];
static int        s_blackCount = 0;
static wchar_t    s_exeDir[MAX_PATH];
static wchar_t    s_wctl[MAX_PATH];
static wchar_t    s_cfg[MAX_PATH];
static wchar_t    s_ini[MAX_PATH];
static HWND       s_hwnd = NULL, s_list = NULL, s_log = NULL;
static HFONT      s_font = NULL;
static BOOL       s_busy = FALSE;
static CRITICAL_SECTION s_logCs;
static wchar_t    s_logBuf[65536];
static int        s_logLen = 0;

/* ------------------------------------------------------------------ */
static void AppendLog(const wchar_t* s)
{
    int n = lstrlenW(s);
    EnterCriticalSection(&s_logCs);
    if (s_logLen + n + 8 < (int)(sizeof(s_logBuf) / sizeof(wchar_t))) {
        lstrcpyW(s_logBuf + s_logLen, s);
        s_logLen += n;
        if (n >= 2 && s[n - 1] != L'\n') {
            lstrcpyW(s_logBuf + s_logLen, L"\r\n");
            s_logLen += 2;
        }
    }
    LeaveCriticalSection(&s_logCs);
    if (s_hwnd) PostMessageW(s_hwnd, WM_APP_OUTPUT, 0, 0);
}

static void LogF(const wchar_t* fmt, ...)
{
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 2047, fmt, ap);
    buf[2047] = 0;
    va_end(ap);
    AppendLog(buf);
}

static void DrainLog(void)
{
    EnterCriticalSection(&s_logCs);
    if (s_logLen > 0) {
        int idx = GetWindowTextLengthW(s_log);
        SendMessageW(s_log, EM_SETSEL, (WPARAM)idx, (LPARAM)idx);
        SendMessageW(s_log, EM_REPLACESEL, FALSE, (LPARAM)s_logBuf);
        s_logLen = 0;
        s_logBuf[0] = 0;
    }
    LeaveCriticalSection(&s_logCs);
}

/* ------------------------------------------------------------------ */
/* 目标进程 / 配置                                                       */
/* ------------------------------------------------------------------ */
static wchar_t* TrimW(wchar_t* s)
{
    wchar_t* p = s;
    wchar_t* e;
    while (*p == L' ' || *p == L'\t') p++;
    e = p + lstrlenW(p);
    while (e > p && (e[-1] == L' ' || e[-1] == L'\t' || e[-1] == L'\r' || e[-1] == L'\n')) e--;
    *e = 0;
    return p;
}

static BOOL LoadBlacklist(void)
{
    HANDLE h;
    DWORD size = 0, rd = 0;
    char* raw;
    wchar_t* txt = NULL;
    wchar_t* line;
    wchar_t* ctx = NULL;

    s_blackCount = 0;
    h = CreateFileW(s_cfg, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        /* 没有就写一份样例 */
        h = CreateFileW(s_cfg, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            const char* sample =
                "# wctl_gui 目标进程: 每行一个进程名(可含 .exe), 也支持 名称|完整路径\n"
                "# 只有在这个名单里的程序才会出现在上面的列表中\n"
                "Student.exe\n"
                "StudentMain.exe\n";
            DWORD w = 0;
            WriteFile(h, sample, (DWORD)lstrlenA(sample), &w, NULL);
            CloseHandle(h);
            LogF(L"已生成示例目标进程: %s", s_cfg);
            h = CreateFileW(s_cfg, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, NULL);
        }
    }
    if (h == INVALID_HANDLE_VALUE) {
        LogF(L"打开目标进程失败: %s (err=%lu)", s_cfg, GetLastError());
        return FALSE;
    }
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size == 0) { CloseHandle(h); LogF(L"目标进程为空: %s", s_cfg); return TRUE; }
    raw = (char*)malloc(size + 1);
    if (!raw) { CloseHandle(h); return FALSE; }
    ReadFile(h, raw, size, &rd, NULL);
    raw[rd] = 0;
    CloseHandle(h);

    /* UTF-8 优先, 失败退回 ANSI */
    {
        int skip = ((unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB &&
                    (unsigned char)raw[2] == 0xBF) ? 3 : 0;
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw + skip, -1, NULL, 0);
        if (n > 0) {
            txt = (wchar_t*)malloc(n * sizeof(wchar_t));
            if (txt) MultiByteToWideChar(CP_UTF8, 0, raw + skip, -1, txt, n);
        } else {
            n = MultiByteToWideChar(CP_ACP, 0, raw, -1, NULL, 0);
            txt = (wchar_t*)malloc((n + 1) * sizeof(wchar_t));
            if (txt) MultiByteToWideChar(CP_ACP, 0, raw, -1, txt, n);
        }
    }
    free(raw);
    if (!txt) return FALSE;

    for (line = wcstok_s(txt, L"\r\n", &ctx); line && s_blackCount < MAX_ENTRIES;
         line = wcstok_s(NULL, L"\r\n", &ctx)) {
        wchar_t* p = TrimW(line);
        wchar_t* bar;
        int n;
        if (!*p || *p == L'#') continue;
        bar = wcschr(p, L'|');
        if (bar) {
            *bar = 0;
            lstrcpynW(s_black[s_blackCount].path, TrimW(bar + 1), MAX_PATH);
        } else {
            s_black[s_blackCount].path[0] = 0;
        }
        p = TrimW(p);
        lstrcpynW(s_black[s_blackCount].name, p, 120);
        n = lstrlenW(s_black[s_blackCount].name);
        if (n > 4 && !lstrcmpiW(s_black[s_blackCount].name + n - 4, L".exe"))
            s_black[s_blackCount].name[n - 4] = 0;
        s_blackCount++;
    }
    free(txt);
    LogF(L"已载入目标进程 %d 条: %s", s_blackCount, s_cfg);
    return TRUE;
}

static int MatchBlack(const wchar_t* exeName)
{
    wchar_t base[128];
    int n, i;
    lstrcpynW(base, exeName, 120);
    n = lstrlenW(base);
    if (n > 4 && !lstrcmpiW(base + n - 4, L".exe")) base[n - 4] = 0;
    for (i = 0; i < s_blackCount; i++)
        if (!lstrcmpiW(base, s_black[i].name)) return i;
    return -1;
}

/* ------------------------------------------------------------------ */
/* 进程 / 注入状态                                                     */
/* ------------------------------------------------------------------ */
typedef struct ProcRow {
    DWORD pid;
    wchar_t name[128];
    wchar_t path[MAX_PATH];
    HWND  hwnd;
    BOOL  injected;
    int   blackIdx;
} ProcRow;

static BOOL IsInjected(DWORD pid)
{
    wchar_t name[64];
    HANDLE h;
    WctlShared* p;
    BOOL ok = FALSE;
    _snwprintf(name, 63, WCTL_SHM_FMT, pid);
    name[63] = 0;
    h = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (!h) return FALSE;
    p = (WctlShared*)MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(WctlShared));
    if (p) {
        ok = (p->magic == WCTL_SHM_MAGIC && p->ready != 0) ? TRUE : FALSE;
        UnmapViewOfFile(p);
    }
    CloseHandle(h);
    return ok;
}

static BOOL CALLBACK FindWinProc(HWND hwnd, LPARAM lp)
{
    ProcRow* row = (ProcRow*)lp;
    DWORD pid = 0;
    LONG_PTR ex;
    RECT r;
    wchar_t cls[64];

    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != row->pid) return TRUE;
    if (GetParent(hwnd)) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return TRUE;
    cls[0] = 0;
    GetClassNameW(hwnd, cls, 63);
    if (lstrcmpiW(cls, L"WctlPanelClsW") == 0) return TRUE;
    if (!GetWindowRect(hwnd, &r)) return TRUE;
    if (r.right - r.left < 40 || r.bottom - r.top < 30) return TRUE;
    row->hwnd = hwnd;
    return FALSE;
}

static int CollectProcs(ProcRow* rows, int maxn)
{
    HANDLE snap;
    PROCESSENTRY32W pe;
    int n = 0;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            int bi = MatchBlack(pe.szExeFile);
            if (bi < 0) continue;
            if (pe.th32ProcessID == GetCurrentProcessId()) continue;
            if (n >= maxn) break;
            rows[n].pid = pe.th32ProcessID;
            lstrcpynW(rows[n].name, pe.szExeFile, 128);
            rows[n].path[0] = 0;
            rows[n].hwnd = NULL;
            rows[n].blackIdx = bi;
            {
                HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                if (hp) {
                    DWORD sz = MAX_PATH;
                    QueryFullProcessImageNameW(hp, 0, rows[n].path, &sz);
                    CloseHandle(hp);
                }
            }
            if (!rows[n].path[0] && s_black[bi].path[0])
                lstrcpynW(rows[n].path, s_black[bi].path, MAX_PATH);
            EnumWindows(FindWinProc, (LPARAM)&rows[n]);
            rows[n].injected = IsInjected(pe.th32ProcessID);
            n++;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return n;
}

/* ------------------------------------------------------------------ */
static void RefreshList(void)
{
    ProcRow rows[512];
    int n, i;
    int sel = -1;
    LVITEMW it;
    wchar_t stbuf[64];

    if (s_busy) return;
    if (ListView_GetSelectedCount(s_list) > 0) {
        sel = ListView_GetNextItem(s_list, -1, LVNI_SELECTED);
    }
    n = CollectProcs(rows, 512);
    /* 名单里但当前没在跑的, 也列出来, 方便"启动并注入" */
    for (i = 0; i < s_blackCount && n < 512; i++) {
        int k, running = 0;
        for (k = 0; k < n; k++) {
            wchar_t base[128];
            int m;
            lstrcpynW(base, rows[k].name, 120);
            m = lstrlenW(base);
            if (m > 4 && !lstrcmpiW(base + m - 4, L".exe")) base[m - 4] = 0;
            if (!lstrcmpiW(base, s_black[i].name)) { running = 1; break; }
        }
        if (running) continue;
        rows[n].pid = 0;
        _snwprintf(rows[n].name, 127, L"%s.exe", s_black[i].name);
        rows[n].name[127] = 0;
        lstrcpynW(rows[n].path, s_black[i].path, MAX_PATH);
        if (!rows[n].path[0]) {
            /* 名单里没写路径时, 先在程序自己所在目录/PATH 里找一找 */
            wchar_t found[MAX_PATH];
            if (SearchPathW(NULL, rows[n].name, NULL, MAX_PATH, found, NULL))
                lstrcpynW(rows[n].path, found, MAX_PATH);
        }
        rows[n].hwnd = NULL;
        rows[n].injected = FALSE;
        rows[n].blackIdx = i;
        n++;
    }

    ListView_DeleteAllItems(s_list);
    for (i = 0; i < n; i++) {
        wchar_t pidbuf[32];
        const wchar_t* st;
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = rows[i].name;
        ListView_InsertItem(s_list, &it);

        if (rows[i].pid == 0) {
            lstrcpynW(pidbuf, L"-", 31);
            st = L"未运行 (可启动并注入)";
        } else {
            _snwprintf(pidbuf, 31, L"%lu", (unsigned long)rows[i].pid);
            pidbuf[31] = 0;
            if (!rows[i].hwnd) st = rows[i].injected ? L"已注入 / 当前无窗口" : L"当前无窗口";
            else if (rows[i].injected) st = L"已注入";
            else st = L"未注入";
        }
        lstrcpynW(stbuf, st, 63);
        ListView_SetItemText(s_list, i, 1, pidbuf);
        ListView_SetItemText(s_list, i, 2, rows[i].path);
        ListView_SetItemText(s_list, i, 3, stbuf);
    }
    if (sel >= 0 && sel < n) {
        ListView_SetItemState(s_list, sel, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
    } else if (n > 0) {
        /* 没有任何选中项时自动选中第一行, 省得每次都点一下 */
        ListView_SetItemState(s_list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
    }
    SetWindowTextW(s_hwnd, L"wctl 控制台 (GUI 外壳) -- 功能由 winctl.exe 执行");
}

static BOOL GetSel(ProcRow* out)
{
    int idx = ListView_GetNextItem(s_list, -1, LVNI_SELECTED);
    wchar_t buf[MAX_PATH];
    if (idx < 0) { LogF(L"请先在列表里选中一个程序"); return FALSE; }
    memset(out, 0, sizeof(*out));
    ListView_GetItemText(s_list, idx, 0, out->name, 128);
    ListView_GetItemText(s_list, idx, 1, buf, 32);
    out->pid = (DWORD)_wtoi(buf);
    ListView_GetItemText(s_list, idx, 2, out->path, MAX_PATH);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* 生成命令行并执行(功能全部在 winctl.exe 里)                           */
/* ------------------------------------------------------------------ */
static void BuildSwitchArgs(wchar_t* out, int cch)
{
    BOOL freeze = (SendMessageW(GetDlgItem(s_hwnd, IDC_CHK_FREEZE), BM_GETCHECK, 0, 0) == BST_CHECKED);
    BOOL top    = (SendMessageW(GetDlgItem(s_hwnd, IDC_CHK_TOP), BM_GETCHECK, 0, 0) == BST_CHECKED);
    BOOL full   = (SendMessageW(GetDlgItem(s_hwnd, IDC_CHK_FULL), BM_GETCHECK, 0, 0) == BST_CHECKED);
    BOOL lock   = (SendMessageW(GetDlgItem(s_hwnd, IDC_CHK_LOCK), BM_GETCHECK, 0, 0) == BST_CHECKED);
    BOOL size   = (SendMessageW(GetDlgItem(s_hwnd, IDC_CHK_SIZE), BM_GETCHECK, 0, 0) == BST_CHECKED);
    BOOL panel  = (SendMessageW(GetDlgItem(s_hwnd, IDC_CHK_PANEL), BM_GETCHECK, 0, 0) == BST_CHECKED);
    BOOL allwin = (SendMessageW(GetDlgItem(s_hwnd, IDC_CHK_ALLWIN), BM_GETCHECK, 0, 0) == BST_CHECKED);
    BOOL verb   = (SendMessageW(GetDlgItem(s_hwnd, IDC_CHK_VERBOSE), BM_GETCHECK, 0, 0) == BST_CHECKED);
    BOOL hideIdle = (SendMessageW(GetDlgItem(s_hwnd, IDC_CHK_HIDEIDLE), BM_GETCHECK, 0, 0) == BST_CHECKED);
    BOOL keepct   = (SendMessageW(GetDlgItem(s_hwnd, IDC_CHK_KEEPCT), BM_GETCHECK, 0, 0) == BST_CHECKED);
    BOOL scalect  = (SendMessageW(GetDlgItem(s_hwnd, IDC_CHK_SCALECT), BM_GETCHECK, 0, 0) == BST_CHECKED);
    BOOL focusSim = (SendMessageW(GetDlgItem(s_hwnd, IDC_CHK_FOCUS), BM_GETCHECK, 0, 0) == BST_CHECKED);
    BOOL remap    = (SendMessageW(GetDlgItem(s_hwnd, IDC_CHK_REMAP), BM_GETCHECK, 0, 0) == BST_CHECKED);
    BOOL possim   = (SendMessageW(GetDlgItem(s_hwnd, IDC_CHK_POSSIM), BM_GETCHECK, 0, 0) == BST_CHECKED);
    BOOL rmsync   = (SendMessageW(GetDlgItem(s_hwnd, IDC_CHK_RMSYNC), BM_GETCHECK, 0, 0) == BST_CHECKED);
    int  posIdx   = (int)SendMessageW(GetDlgItem(s_hwnd, IDC_CMB_PANELPOS), CB_GETCURSEL, 0, 0);
    wchar_t wbuf[32];
    wchar_t t[64];

    out[0] = 0;
    if (freeze) {
        /* 锁定窗口状态 = 勾选那一刻的置顶/位置/大小/全屏全部固化 */
        lstrcatW(out, L" --freeze=on");
    } else {
        lstrcatW(out, top  ? L" --topmost=on"    : L" --topmost=off");
        lstrcatW(out, full ? L" --fullscreen=on" : L" --fullscreen=off");
        lstrcatW(out, lock ? L" --lockmove=user" : L" --lockmove=off");
        lstrcatW(out, size ? L" --locksize=on"   : L" --locksize=off");
    }
    lstrcatW(out, panel ? L" --panel=on" : L" --panel=off");
    if (panel) {
        /* 面板位置 / 宽度 / 平时是否隐藏 */
        static const wchar_t* posName[4] = { L"left", L"center", L"right", L"custom" };
        if (posIdx >= 0 && posIdx < 4) {
            _snwprintf(t, 63, L" --panel-pos=%s", posName[posIdx]);
            t[63] = 0;
            lstrcatW(out, t);
        }
        wbuf[0] = 0;
        GetWindowTextW(GetDlgItem(s_hwnd, IDC_EDT_PANELW), wbuf, 31);
        if (_wtoi(wbuf) > 0) {
            _snwprintf(t, 63, L" --panel-width=%d", _wtoi(wbuf));
            t[63] = 0;
            lstrcatW(out, t);
        }
        lstrcatW(out, hideIdle ? L" --panel-alpha=0" : L" --panel-alpha=110");
    }
    lstrcatW(out, scalect ? L" --scale-content=on" : L" --scale-content=off");
    lstrcatW(out, keepct ? L" --keep-content=on" : L" --keep-content=off");
    lstrcatW(out, focusSim ? L" --focus=on" : L" --focus=off");
    lstrcatW(out, remap ? L" --remap=on" : L" --remap=off");
    if (remap) {
        lstrcatW(out, possim ? L" --possim=on" : L" --possim=off");
        lstrcatW(out, rmsync ? L" --remap-scale=sync" : L" --remap-scale=full");
    }
    if (allwin) lstrcatW(out, L" --all-windows");
    if (verb)   lstrcatW(out, L" --verbose");
    (void)cch;
}

typedef struct Job {
    wchar_t cmd[5000];
} Job;

static DWORD WINAPI JobThread(LPVOID param)
{
    Job* j = (Job*)param;
    SECURITY_ATTRIBUTES sa;
    HANDLE rp = NULL, wp = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t cmd[5000];
    char buf[1024];
    DWORD rd;
    BOOL ok;

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rp, &wp, &sa, 0)) {
        LogF(L"创建管道失败 err=%lu", GetLastError());
        goto done;
    }
    SetHandleInformation(rp, HANDLE_FLAG_INHERIT, 0);

    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wp;
    si.hStdError = wp;
    si.hStdInput = NULL;

    lstrcpynW(cmd, j->cmd, 5000);
    ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(wp);
    wp = NULL;
    if (!ok) {
        LogF(L"启动命令失败 err=%lu", GetLastError());
        CloseHandle(rp);
        goto done;
    }
    /* 等它结束(边读边等, 避免管道写满卡住) */
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(rp, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            if (ReadFile(rp, buf, sizeof(buf) - 1, &rd, NULL) && rd > 0) {
                int wn;
                wchar_t* wb;
                buf[rd] = 0;
                wn = MultiByteToWideChar(CP_UTF8, 0, buf, -1, NULL, 0);
                wb = (wchar_t*)malloc(wn * sizeof(wchar_t));
                if (wb) {
                    MultiByteToWideChar(CP_UTF8, 0, buf, -1, wb, wn);
                    AppendLog(wb);
                    free(wb);
                }
                continue;
            }
        }
        if (WaitForSingleObject(pi.hProcess, 30) == WAIT_OBJECT_0) {
            while (PeekNamedPipe(rp, NULL, 0, NULL, &avail, NULL) && avail > 0) {
                if (!ReadFile(rp, buf, sizeof(buf) - 1, &rd, NULL) || rd == 0) break;
                buf[rd] = 0;
                {
                    int wn = MultiByteToWideChar(CP_UTF8, 0, buf, -1, NULL, 0);
                    wchar_t* wb = (wchar_t*)malloc(wn * sizeof(wchar_t));
                    if (wb) { MultiByteToWideChar(CP_UTF8, 0, buf, -1, wb, wn); AppendLog(wb); free(wb); }
                }
            }
            break;
        }
    }
    {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        LogF(L"[命令结束, 退出码 %lu]", (unsigned long)code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rp);

done:
    if (wp) CloseHandle(wp);
    free(j);
    if (s_hwnd) PostMessageW(s_hwnd, WM_APP_DONE, 0, 0);
    return 0;
}

static void RunWctl(const wchar_t* args);
static void StartNextJob(void);

/* 简单的命令队列: 队列里只存 winctl.exe 的参数部分 */
#define QUEUE_MAX 4
static wchar_t s_queue[QUEUE_MAX][5000];
static int     s_qHead = 0, s_qTail = 0;

static void PushJob(const wchar_t* args)
{
    int next = (s_qTail + 1) % QUEUE_MAX;
    if (next == s_qHead) { LogF(L"命令队列已满, 丢弃: %s", args); return; }
    lstrcpynW(s_queue[s_qTail], args, 5000);
    s_qTail = next;
    if (!s_busy) StartNextJob();
}

static void StartNextJob(void)
{
    Job* j;
    HANDLE th;
    wchar_t* args;
    if (s_busy) return;
    if (s_qHead == s_qTail) return;
    args = s_queue[s_qHead];
    s_qHead = (s_qHead + 1) % QUEUE_MAX;

    j = (Job*)calloc(1, sizeof(Job));
    if (!j) return;
    _snwprintf(j->cmd, 4999, L"\"%s\" %s", s_wctl, args);
    j->cmd[4999] = 0;
    LogF(L"> %s", j->cmd);
    s_busy = TRUE;
    EnableWindow(GetDlgItem(s_hwnd, IDC_BTN_ATTACH), FALSE);
    EnableWindow(GetDlgItem(s_hwnd, IDC_BTN_KILL), FALSE);
    EnableWindow(GetDlgItem(s_hwnd, IDC_BTN_RESTART), FALSE);
    EnableWindow(GetDlgItem(s_hwnd, IDC_BTN_DETACH), FALSE);
    th = CreateThread(NULL, 0, JobThread, j, 0, NULL);
    if (th) CloseHandle(th);
    else { s_busy = FALSE; free(j); }
}

static void RunWctl(const wchar_t* args)
{
    PushJob(args);
}

/* ------------------------------------------------------------------ */
static void Layout(void)
{
    RECT rc;
    int w, h, y;
    int pad = 10;
    if (!s_hwnd) return;
    GetClientRect(s_hwnd, &rc);
    w = rc.right;
    h = rc.bottom;

    MoveWindow(GetDlgItem(s_hwnd, IDC_CFGPATH), pad, 8, w - 3 * pad - 250, 22, TRUE);
    MoveWindow(GetDlgItem(s_hwnd, IDC_BTN_RELOAD), w - 3 * pad - 244, 8, 78, 22, TRUE);
    MoveWindow(GetDlgItem(s_hwnd, IDC_BTN_EDITCFG), w - 3 * pad - 162, 8, 78, 22, TRUE);
    MoveWindow(GetDlgItem(s_hwnd, IDC_BTN_REFRESH), w - 3 * pad - 80, 8, 74, 22, TRUE);

    y = 38;
    {
        int listH = (h - y - 210);
        if (listH < 120) listH = 120;
        MoveWindow(s_list, pad, y, w - 2 * pad, listH, TRUE);
        y += listH + 8;
    }
    /* 开关行 */
    MoveWindow(GetDlgItem(s_hwnd, IDC_CHK_FREEZE), pad, y, 330, 22, TRUE);
    MoveWindow(GetDlgItem(s_hwnd, IDC_CHK_TOP), pad + 340, y, 80, 22, TRUE);
    MoveWindow(GetDlgItem(s_hwnd, IDC_CHK_FULL), pad + 420, y, 80, 22, TRUE);
    MoveWindow(GetDlgItem(s_hwnd, IDC_CHK_LOCK), pad + 500, y, 100, 22, TRUE);
    MoveWindow(GetDlgItem(s_hwnd, IDC_CHK_SIZE), pad + 600, y, 100, 22, TRUE);
    MoveWindow(GetDlgItem(s_hwnd, IDC_CHK_PANEL), pad + 700, y, 90, 22, TRUE);
    y += 26;
    MoveWindow(GetDlgItem(s_hwnd, IDC_CHK_ALLWIN), pad + 200, y, 130, 22, TRUE);
    MoveWindow(GetDlgItem(s_hwnd, IDC_CHK_VERBOSE), pad + 330, y, 130, 22, TRUE);
    MoveWindow(GetDlgItem(s_hwnd, IDC_CHK_HIDEIDLE), pad + 460, y, 140, 22, TRUE);
    MoveWindow(GetDlgItem(s_hwnd, IDC_CHK_KEEPCT), pad + 600, y, 240, 22, TRUE);
    y += 26;
    MoveWindow(GetDlgItem(s_hwnd, IDC_CHK_SCALECT), pad + 200, y, 300, 22, TRUE);
    MoveWindow(GetDlgItem(s_hwnd, IDC_CHK_FOCUS), pad + 508, y, 80, 22, TRUE);
    MoveWindow(GetDlgItem(s_hwnd, IDC_CHK_REMAP), pad + 592, y, 100, 22, TRUE);
    MoveWindow(GetDlgItem(s_hwnd, IDC_CHK_POSSIM), pad + 696, y, 90, 22, TRUE);
    MoveWindow(GetDlgItem(s_hwnd, IDC_CHK_RMSYNC), pad + 790, y, 150, 22, TRUE);
    y += 26;
    /* 面板位置 / 宽度 */
    MoveWindow(GetDlgItem(s_hwnd, IDC_LBL_PANELPOS), pad + 200, y + 4, 70, 20, TRUE);
    MoveWindow(GetDlgItem(s_hwnd, IDC_CMB_PANELPOS), pad + 272, y, 120, 200, TRUE);
    MoveWindow(GetDlgItem(s_hwnd, IDC_LBL_PANELW), pad + 400, y + 4, 70, 20, TRUE);
    MoveWindow(GetDlgItem(s_hwnd, IDC_EDT_PANELW), pad + 472, y, 70, 22, TRUE);
    y += 28;
    /* 按钮行 */
    MoveWindow(GetDlgItem(s_hwnd, IDC_BTN_ATTACH), pad, y, 130, 30, TRUE);
    MoveWindow(GetDlgItem(s_hwnd, IDC_BTN_START), pad + 138, y, 130, 30, TRUE);
    MoveWindow(GetDlgItem(s_hwnd, IDC_BTN_KILL), pad + 276, y, 100, 30, TRUE);
    MoveWindow(GetDlgItem(s_hwnd, IDC_BTN_RESTART), pad + 384, y, 130, 30, TRUE);
    MoveWindow(GetDlgItem(s_hwnd, IDC_BTN_DETACH), pad + 522, y, 120, 30, TRUE);
    MoveWindow(GetDlgItem(s_hwnd, IDC_BTN_FOLDER), pad + 650, y, 140, 30, TRUE);
    y += 36;
    MoveWindow(s_log, pad, y, w - 2 * pad, h - y - pad, TRUE);
}

static void SyncEnable(void)
{
    BOOL freeze = (SendMessageW(GetDlgItem(s_hwnd, IDC_CHK_FREEZE), BM_GETCHECK, 0, 0) == BST_CHECKED);
    BOOL scale  = (SendMessageW(GetDlgItem(s_hwnd, IDC_CHK_SCALECT), BM_GETCHECK, 0, 0) == BST_CHECKED);
    BOOL remap  = (SendMessageW(GetDlgItem(s_hwnd, IDC_CHK_REMAP), BM_GETCHECK, 0, 0) == BST_CHECKED);
    int ids[4];
    int i;
    ids[0] = IDC_CHK_TOP;
    ids[1] = IDC_CHK_FULL;
    ids[2] = IDC_CHK_LOCK;
    ids[3] = IDC_CHK_SIZE;
    for (i = 0; i < 4; i++) EnableWindow(GetDlgItem(s_hwnd, ids[i]), !freeze);
    /* 内容缩放开着的时候, "保持原尺寸"只是它失效时的退路, 灰掉免得误解;
       显示重映射是第二种实现, 与内容缩放互斥 */
    EnableWindow(GetDlgItem(s_hwnd, IDC_CHK_KEEPCT), !scale && !remap);
    EnableWindow(GetDlgItem(s_hwnd, IDC_CHK_SCALECT), !remap);
    /* 位置模拟 / 同步缩放只在重映射时有意义 */
    EnableWindow(GetDlgItem(s_hwnd, IDC_CHK_POSSIM), remap);
    EnableWindow(GetDlgItem(s_hwnd, IDC_CHK_RMSYNC), remap);
}

static void SaveUiState(void)
{
    int ids[15];
    const wchar_t* keys[15];
    int i;
    wchar_t buf[32];
    ids[0] = IDC_CHK_FREEZE;  keys[0] = L"freeze";
    ids[1] = IDC_CHK_TOP;     keys[1] = L"topmost";
    ids[2] = IDC_CHK_FULL;    keys[2] = L"fullscreen";
    ids[3] = IDC_CHK_LOCK;    keys[3] = L"lockmove";
    ids[4] = IDC_CHK_SIZE;    keys[4] = L"locksize";
    ids[5] = IDC_CHK_PANEL;   keys[5] = L"panel";
    ids[6] = IDC_CHK_ALLWIN;  keys[6] = L"allwindows";
    ids[7] = IDC_CHK_VERBOSE; keys[7] = L"verbose";
    ids[8] = IDC_CHK_HIDEIDLE;keys[8] = L"hideidle";
    ids[9] = IDC_CHK_KEEPCT;  keys[9] = L"keepcontent";
    ids[10]= IDC_CHK_SCALECT; keys[10]= L"scalecontent";
    ids[11]= IDC_CHK_FOCUS;   keys[11]= L"focus";
    ids[12]= IDC_CHK_REMAP;   keys[12]= L"remap";
    ids[13]= IDC_CHK_POSSIM;  keys[13]= L"possim";
    ids[14]= IDC_CHK_RMSYNC;  keys[14]= L"remapsync";
    for (i = 0; i < 15; i++) {
        BOOL on = (SendMessageW(GetDlgItem(s_hwnd, ids[i]), BM_GETCHECK, 0, 0) == BST_CHECKED);
        WritePrivateProfileStringW(L"gui", keys[i], on ? L"1" : L"0", s_ini);
    }
    /* 面板位置 / 宽度 */
    {
        int sel = (int)SendMessageW(GetDlgItem(s_hwnd, IDC_CMB_PANELPOS), CB_GETCURSEL, 0, 0);
        _snwprintf(buf, 31, L"%d", sel < 0 ? 1 : sel);
        buf[31] = 0;
        WritePrivateProfileStringW(L"gui", L"panelpos", buf, s_ini);
    }
    GetWindowTextW(GetDlgItem(s_hwnd, IDC_EDT_PANELW), buf, 31);
    WritePrivateProfileStringW(L"gui", L"panelwidth", buf, s_ini);
}

static void LoadUiState(void)
{
    int ids[15];
    const wchar_t* keys[15];
    int i;
    wchar_t buf[32];
    ids[0] = IDC_CHK_FREEZE;  keys[0] = L"freeze";
    ids[1] = IDC_CHK_TOP;     keys[1] = L"topmost";
    ids[2] = IDC_CHK_FULL;    keys[2] = L"fullscreen";
    ids[3] = IDC_CHK_LOCK;    keys[3] = L"lockmove";
    ids[4] = IDC_CHK_SIZE;    keys[4] = L"locksize";
    ids[5] = IDC_CHK_PANEL;   keys[5] = L"panel";
    ids[6] = IDC_CHK_ALLWIN;  keys[6] = L"allwindows";
    ids[7] = IDC_CHK_VERBOSE; keys[7] = L"verbose";
    ids[8] = IDC_CHK_HIDEIDLE;keys[8] = L"hideidle";
    ids[9] = IDC_CHK_KEEPCT;  keys[9] = L"keepcontent";
    ids[10]= IDC_CHK_SCALECT; keys[10]= L"scalecontent";
    ids[11]= IDC_CHK_FOCUS;   keys[11]= L"focus";
    ids[12]= IDC_CHK_REMAP;   keys[12]= L"remap";
    ids[13]= IDC_CHK_POSSIM;  keys[13]= L"possim";
    ids[14]= IDC_CHK_RMSYNC;  keys[14]= L"remapsync";
    for (i = 0; i < 15; i++) {
        int def = 0;
        if (ids[i] == IDC_CHK_PANEL)    def = 1;
        if (ids[i] == IDC_CHK_HIDEIDLE) def = 1;   /* 默认: 鼠标不在上面时完全隐藏 */
        if (ids[i] == IDC_CHK_KEEPCT)   def = 1;
        if (ids[i] == IDC_CHK_SCALECT)  def = 1;   /* 默认开: 远端画面等比缩放显示 */
        if (ids[i] == IDC_CHK_POSSIM)   def = 1;   /* 默认开: 窗口位置模拟 */
        GetPrivateProfileStringW(L"gui", keys[i], def ? L"1" : L"0", buf, 32, s_ini);
        SendMessageW(GetDlgItem(s_hwnd, ids[i]), BM_SETCHECK,
                     (buf[0] == L'1') ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    GetPrivateProfileStringW(L"gui", L"panelpos", L"1", buf, 32, s_ini);
    SendMessageW(GetDlgItem(s_hwnd, IDC_CMB_PANELPOS), CB_SETCURSEL, (WPARAM)_wtoi(buf), 0);
    GetPrivateProfileStringW(L"gui", L"panelwidth", L"0", buf, 32, s_ini);
    SetWindowTextW(GetDlgItem(s_hwnd, IDC_EDT_PANELW), buf);
}

static void DoAttach(void)
{
    ProcRow r;
    wchar_t args[2048], sw[1024];
    if (!GetSel(&r)) return;
    if (!r.pid) { LogF(L"这一项当前没在运行, 请用『启动并注入』"); return; }
    BuildSwitchArgs(sw, 1024);
    _snwprintf(args, 2047, L"--attach %lu%s", (unsigned long)r.pid, sw);
    args[2047] = 0;
    RunWctl(args);
}

static void DoKill(void)
{
    ProcRow r;
    wchar_t args[256];
    if (!GetSel(&r)) return;
    if (!r.pid) { LogF(L"这一项当前没在运行"); return; }
    _snwprintf(args, 255, L"--kill %lu", (unsigned long)r.pid);
    args[255] = 0;
    RunWctl(args);
}

static void DoDetach(void)
{
    ProcRow r;
    wchar_t args[256];
    if (!GetSel(&r)) return;
    if (!r.pid) { LogF(L"这一项当前没在运行"); return; }
    _snwprintf(args, 255, L"--detach %lu", (unsigned long)r.pid);
    args[255] = 0;
    RunWctl(args);
}

/* 先用 winctl --kill 结束, 再按记录的路径启动并注入*/
static void DoRestart(void)
{
    ProcRow r;
    wchar_t sw[1024], args[4096];
    if (!GetSel(&r)) return;
    if (!r.path[0]) { LogF(L"拿不到这个进程的完整路径, 无法重启(可在目标进程里写 名称|完整路径)"); return; }
    BuildSwitchArgs(sw, 1024);
    if (r.pid) {
        _snwprintf(args, 4095, L"--kill %lu", (unsigned long)r.pid);
        args[4095] = 0;
        RunWctl(args);
    }
    _snwprintf(args, 4095, L"\"%s\"%s", r.path, sw);
    args[4095] = 0;
    RunWctl(args);
}

static void DoStartNew(void)
{
    ProcRow r;
    wchar_t sw[1024], args[4096];
    if (!GetSel(&r)) return;
    if (!r.path[0]) { LogF(L"拿不到完整路径, 无法启动"); return; }
    BuildSwitchArgs(sw, 1024);
    _snwprintf(args, 4095, L"\"%s\"%s", r.path, sw);
    args[4095] = 0;
    RunWctl(args);
}

static void DoOpenFolder(void)
{
    ProcRow r;
    wchar_t arg[MAX_PATH + 32];
    if (!GetSel(&r)) return;
    if (!r.path[0]) { LogF(L"拿不到完整路径"); return; }
    _snwprintf(arg, MAX_PATH + 31, L"/select,\"%s\"", r.path);
    arg[MAX_PATH + 31] = 0;
    ShellExecuteW(NULL, L"open", L"explorer.exe", arg, NULL, SW_SHOWNORMAL);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        NONCLIENTMETRICSW ncm;
        HWND h;
        int i;
        s_hwnd = hwnd;     
        static const struct { int id; const wchar_t* text; DWORD style; } chk[] = {
            { IDC_CHK_FREEZE,  L"锁定窗口状态(禁 最小化/最大化/全屏/移动/缩放)", BS_AUTOCHECKBOX },
            { IDC_CHK_TOP,     L"置顶",     BS_AUTOCHECKBOX },
            { IDC_CHK_FULL,    L"全屏",     BS_AUTOCHECKBOX },
            { IDC_CHK_LOCK,    L"移动锁",   BS_AUTOCHECKBOX },
            { IDC_CHK_SIZE,    L"锁定大小", BS_AUTOCHECKBOX },
            { IDC_CHK_PANEL,   L"显示面板", BS_AUTOCHECKBOX },
            { IDC_CHK_ALLWIN,  L"多窗口(全部接管)", BS_AUTOCHECKBOX },
            { IDC_CHK_VERBOSE, L"详细日志", BS_AUTOCHECKBOX },
            { IDC_CHK_HIDEIDLE,L"面板平时隐藏", BS_AUTOCHECKBOX },
            { IDC_CHK_KEEPCT,  L"内容不缩放时保持原尺寸", BS_AUTOCHECKBOX },
            { IDC_CHK_SCALECT, L"远端画面等比缩放(不裁画面)", BS_AUTOCHECKBOX },
            { IDC_CHK_FOCUS,   L"焦点模拟", BS_AUTOCHECKBOX },
            { IDC_CHK_REMAP,   L"显示重映射", BS_AUTOCHECKBOX },
            { IDC_CHK_POSSIM,  L"位置模拟", BS_AUTOCHECKBOX },
            { IDC_CHK_RMSYNC,  L"重映射同步缩放", BS_AUTOCHECKBOX },
        };
        static const struct { int id; const wchar_t* text; } btn[] = {
            { IDC_BTN_RELOAD,  L"重载名单" },
            { IDC_BTN_REFRESH, L"刷新" },
            { IDC_BTN_EDITCFG, L"编辑名单" },
            { IDC_BTN_ATTACH,  L"附加并注入" },
            { IDC_BTN_START,   L"启动并注入" },
            { IDC_BTN_KILL,    L"强制关闭" },
            { IDC_BTN_RESTART, L"重启并注入" },
            { IDC_BTN_DETACH,  L"卸载还原" },
            { IDC_BTN_FOLDER,  L"打开所在目录" },
        };
        memset(&ncm, 0, sizeof(ncm));
        ncm.cbSize = sizeof(ncm);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        s_font = CreateFontIndirectW(&ncm.lfMessageFont);

        h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", s_cfg,
                            WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL,
                            0, 0, 10, 10, hwnd, (HMENU)IDC_CFGPATH, NULL, NULL);
        SendMessageW(h, WM_SETFONT, (WPARAM)s_font, TRUE);

        s_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                 WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL |
                                 LVS_SHOWSELALWAYS,
                                 0, 0, 10, 10, hwnd, (HMENU)IDC_LIST, NULL, NULL);
        SendMessageW(s_list, WM_SETFONT, (WPARAM)s_font, TRUE);
        ListView_SetExtendedListViewStyle(s_list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        {
            LVCOLUMNW c;
            static const struct { const wchar_t* t; int w; } cols[] = {
                { L"进程名", 190 }, { L"PID", 70 }, { L"完整路径", 470 }, { L"状态", 190 }
            };
            memset(&c, 0, sizeof(c));
            c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            for (i = 0; i < 4; i++) {
                c.pszText = (LPWSTR)cols[i].t;
                c.cx = cols[i].w;
                c.iSubItem = i;
                ListView_InsertColumn(s_list, i, &c);
            }
        }

        s_log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                                ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                0, 0, 10, 10, hwnd, (HMENU)IDC_LOG, NULL, NULL);
        SendMessageW(s_log, WM_SETFONT, (WPARAM)s_font, TRUE);

        /* 面板位置下拉框 + 宽度输入框 + 两个说明标签 */
        {
            HWND cmb = CreateWindowExW(0, L"COMBOBOX", L"",
                                       WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                       0, 0, 10, 200, hwnd, (HMENU)IDC_CMB_PANELPOS, NULL, NULL);
            static const wchar_t* items[4] = { L"靠左", L"居中", L"靠右", L"自定义" };
            int k;
            SendMessageW(cmb, WM_SETFONT, (WPARAM)s_font, TRUE);
            for (k = 0; k < 4; k++) SendMessageW(cmb, CB_ADDSTRING, 0, (LPARAM)items[k]);
            SendMessageW(cmb, CB_SETCURSEL, 1, 0);       /* 默认居中 */
        }
        h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0",
                            WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_CENTER,
                            0, 0, 10, 10, hwnd, (HMENU)IDC_EDT_PANELW, NULL, NULL);
        SendMessageW(h, WM_SETFONT, (WPARAM)s_font, TRUE);
        {
            static const struct { int id; const wchar_t* text; } lbl[] = {
                { IDC_LBL_PANELPOS, L"面板位置:" },
                { IDC_LBL_PANELW,   L"面板宽度:" },
            };
            for (i = 0; i < (int)(sizeof(lbl) / sizeof(lbl[0])); i++) {
                h = CreateWindowExW(0, L"STATIC", lbl[i].text, WS_CHILD | WS_VISIBLE,
                                    0, 0, 10, 20, hwnd, (HMENU)(INT_PTR)lbl[i].id, NULL, NULL);
                SendMessageW(h, WM_SETFONT, (WPARAM)s_font, TRUE);
            }
        }

        for (i = 0; i < (int)(sizeof(btn) / sizeof(btn[0])); i++) {
            h = CreateWindowExW(0, L"BUTTON", btn[i].text,
                                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)btn[i].id, NULL, NULL);
            SendMessageW(h, WM_SETFONT, (WPARAM)s_font, TRUE);
        }
        for (i = 0; i < (int)(sizeof(chk) / sizeof(chk[0])); i++) {
            h = CreateWindowExW(0, L"BUTTON", chk[i].text,
                                WS_CHILD | WS_VISIBLE | chk[i].style,
                                0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)chk[i].id, NULL, NULL);
            SendMessageW(h, WM_SETFONT, (WPARAM)s_font, TRUE);
        }
        LoadUiState();
        SyncEnable();
        SetTimer(hwnd, 1, 3000, NULL);
        Layout();           /* 窗口大小固定时 WM_SIZE 可能早于子控件创建 */
        LogF(L"wctl 控制台已启动。");
        LogF(L"调用命令行程序: %s", s_wctl);
        LoadBlacklist();
        RefreshList();
        return 0;
    }

    case WM_SIZE:
        Layout();
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize.x = 820;
        mm->ptMinTrackSize.y = 560;
        return 0;
    }

    case WM_TIMER:
        if (wp == 1 && !s_busy) RefreshList();
        return 0;

    case WM_APP_OUTPUT:
        DrainLog();
        return 0;

    case WM_APP_DONE:
        s_busy = FALSE;
        EnableWindow(GetDlgItem(s_hwnd, IDC_BTN_ATTACH), TRUE);
        EnableWindow(GetDlgItem(s_hwnd, IDC_BTN_KILL), TRUE);
        EnableWindow(GetDlgItem(s_hwnd, IDC_BTN_RESTART), TRUE);
        EnableWindow(GetDlgItem(s_hwnd, IDC_BTN_DETACH), TRUE);
        DrainLog();
        RefreshList();
        StartNextJob();    
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_CHK_FREEZE:
        case IDC_CHK_TOP:
        case IDC_CHK_FULL:
        case IDC_CHK_LOCK:
        case IDC_CHK_SIZE:
        case IDC_CHK_PANEL:
        case IDC_CHK_ALLWIN:
        case IDC_CHK_VERBOSE:
        case IDC_CHK_HIDEIDLE:
        case IDC_CHK_KEEPCT:
        case IDC_CHK_SCALECT:
        case IDC_CHK_FOCUS:
        case IDC_CHK_REMAP:
        case IDC_CHK_POSSIM:
        case IDC_CHK_RMSYNC:
        case IDC_CMB_PANELPOS:
            SyncEnable();
            SaveUiState();
            return 0;
        case IDC_EDT_PANELW:
            if (HIWORD(wp) == EN_KILLFOCUS) SaveUiState();
            return 0;
        case IDC_BTN_RELOAD:
            LoadBlacklist();
            RefreshList();
            return 0;
        case IDC_BTN_REFRESH:
            RefreshList();
            return 0;
        case IDC_BTN_EDITCFG:
            ShellExecuteW(NULL, L"open", L"notepad.exe", s_cfg, NULL, SW_SHOWNORMAL);
            return 0;
        case IDC_BTN_ATTACH:
            DoAttach();
            return 0;
        case IDC_BTN_START:
            DoStartNew();
            return 0;
        case IDC_BTN_KILL:
            if (MessageBoxW(hwnd, L"强制结束选中进程?\n(先 WM_CLOSE, 不接受就 TerminateProcess)",
                            L"确认", MB_ICONWARNING | MB_YESNO) == IDYES)
                DoKill();
            return 0;
        case IDC_BTN_RESTART:
            DoRestart();
            return 0;
        case IDC_BTN_DETACH:
            DoDetach();
            return 0;
        case IDC_BTN_FOLDER:
            DoOpenFolder();
            return 0;
        default:
            break;
        }
        return 0;

    case WM_CLOSE:
        SaveUiState();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE prev, LPWSTR cmdline, int show)
{
    WNDCLASSEXW wc;
    MSG msg;
    INITCOMMONCONTROLSEX icc;
    HWND hwnd;
    wchar_t* slash;

    (void)prev; (void)cmdline; (void)show;
    InitializeCriticalSection(&s_logCs);

    GetModuleFileNameW(NULL, s_exeDir, MAX_PATH);
    slash = s_exeDir;
    for (wchar_t* q = s_exeDir; *q; q++) if (*q == L'\\' || *q == L'/') slash = q;
    *(slash + 1) = 0;
    lstrcpynW(s_wctl, s_exeDir, MAX_PATH);
    lstrcatW(s_wctl, L"winctl.exe");
    lstrcpynW(s_cfg, s_exeDir, MAX_PATH);
    lstrcatW(s_cfg, L"blacklist.txt");
    lstrcpynW(s_ini, s_exeDir, MAX_PATH);
    lstrcatW(s_ini, L"wctl_gui.ini");

    if (GetFileAttributesW(s_wctl) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(NULL, L"同目录下找不到 winctl.exe。\n请先运行 build.cmd 构建。",
                    L"wctl 控制台", MB_ICONERROR);
        return 1;
    }

    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"WctlGuiWnd";
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    RegisterClassExW(&wc);

    hwnd = CreateWindowExW(0, L"WctlGuiWnd", L"wctl 控制台 (GUI 外壳) -- 功能由 winctl.exe 执行",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN,
                           CW_USEDEFAULT, CW_USEDEFAULT, 980, 720,
                           NULL, NULL, hinst, NULL);
    if (!hwnd) return 1;
    s_hwnd = hwnd;

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DeleteCriticalSection(&s_logCs);
    return 0;
}
