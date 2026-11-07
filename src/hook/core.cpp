/* core.cpp -- 状态机 / 窗口发现 / 三项功能的应用逻辑 / 主循环 */
#include "wctl_core.h"
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

/* ------------------------------------------------------------------ */
/* 全局                                                               */
/* ------------------------------------------------------------------ */
HINSTANCE        g_hinst = NULL;
WctlShared*      g_shm = NULL;
HANDLE           g_shmMap = NULL;
HANDLE           g_tickEvent = NULL;
HWND             g_panel = NULL;
HWND             g_primary = NULL;
CRITICAL_SECTION g_cs;
WndCtx*          g_ctxList = NULL;
volatile LONG    g_stop = 0;
volatile LONG    g_detached = 0;
DWORD            g_tickThreadId = 0;

static HANDLE       s_logFile = INVALID_HANDLE_VALUE;
static volatile LONG s_verbose = 0;
static volatile LONG s_lockEpoch = 1;
static wchar_t      s_exeName[64] = L"?";
static volatile LONG s_clearCount = 0;    /* 兑底取消置顶的累计次数 */
static volatile LONG s_lastClearAt = 0;   /* 最近一次"取消置顶"的时刻 */
static volatile LONG s_fastUntil = 0;     /* 快周期截止时刻(GetTickCount) */
static DWORD        s_curInterval = WCTL_TICK_MS;
static volatile LONG s_killActive = 0;
static volatile LONG s_killAt = 0;
/* 窗口销毁时按"窗口类名"留一份几何备忘(见 GravePut/GraveTake) */
static BOOL         s_freezeOn = FALSE;

/* 窗口销毁时按"窗口类名"留一份几何备忘: 只有同一类名的窗口重新出现(即重建)才继承,
   这样多窗口程序里另一个窗口不会被套用成同一个位置/大小。 */
#define WCTL_GRAVE_MAX 8
typedef struct GeomMemo {
    wchar_t cls[64];
    POINT   pos;
    LONG    w, h;
    BOOL    used;
} GeomMemo;
static GeomMemo s_grave[WCTL_GRAVE_MAX];

static void GravePut(HWND hwnd, const POINT* pos, BOOL hasPos, LONG w, LONG h, BOOL hasSize)
{
    wchar_t cls[64];
    int i, slot = -1;
    cls[0] = 0;
    GetClassNameW(hwnd, cls, 63);
    if (!cls[0]) return;
    for (i = 0; i < WCTL_GRAVE_MAX; i++) {
        if (s_grave[i].used && lstrcmpiW(s_grave[i].cls, cls) == 0) { slot = i; break; }
        if (!s_grave[i].used && slot < 0) slot = i;
    }
    if (slot < 0) slot = 0;
    lstrcpynW(s_grave[slot].cls, cls, 63);
    s_grave[slot].pos = *pos;
    s_grave[slot].w = hasSize ? w : 0;
    s_grave[slot].h = hasSize ? h : 0;
    s_grave[slot].used = TRUE;
}

static BOOL GraveTake(HWND hwnd, POINT* pos, LONG* w, LONG* h)
{
    wchar_t cls[64];
    int i;
    cls[0] = 0;
    GetClassNameW(hwnd, cls, 63);
    if (!cls[0]) return FALSE;
    for (i = 0; i < WCTL_GRAVE_MAX; i++) {
        if (s_grave[i].used && lstrcmpiW(s_grave[i].cls, cls) == 0) {
            *pos = s_grave[i].pos;
            *w = s_grave[i].w;
            *h = s_grave[i].h;
            s_grave[i].used = FALSE;
            return TRUE;
        }
    }
    return FALSE;
}

#define WCTL_MAX_CTX 256

/* ------------------------------------------------------------------ */
/* 日志                                                               */
/* ------------------------------------------------------------------ */
static void LogLine(const wchar_t* text)
{
    if (s_logFile == INVALID_HANDLE_VALUE) {
        wchar_t tmp[MAX_PATH], path[MAX_PATH + 64];
        DWORD n = GetTempPathW(MAX_PATH, tmp);
        if (n == 0 || n > MAX_PATH) return;
        _snwprintf(path, MAX_PATH + 63, L"%swctl-%lu.log", tmp, GetCurrentProcessId());
        path[MAX_PATH + 63] = 0;
        s_logFile = CreateFileW(path, FILE_APPEND_DATA,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (s_logFile != INVALID_HANDLE_VALUE && GetLastError() != ERROR_ALREADY_EXISTS) {
            const unsigned char bom[2] = { 0xFF, 0xFE };
            DWORD bw = 0;
            WriteFile(s_logFile, bom, 2, &bw, NULL);
        }
    }
    if (s_logFile != INVALID_HANDLE_VALUE) {
        SYSTEMTIME st;
        wchar_t line[1400];
        wchar_t head[64];
        DWORD wr = 0;
        int n;
        GetLocalTime(&st);
        _snwprintf(head, 63, L"[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute,
                   st.wSecond, st.wMilliseconds);
        head[63] = 0;
        lstrcpynW(line, head, 1400);
        n = lstrlenW(line);
        lstrcpynW(line + n, text, 1200);
        n = lstrlenW(line);
        line[n++] = L'\r';
        line[n++] = L'\n';
        WriteFile(s_logFile, line, (DWORD)(n * sizeof(wchar_t)), &wr, NULL);
    }
    OutputDebugStringW(text);
    OutputDebugStringW(L"\n");
}

void WctlLog(const wchar_t* fmt, ...)
{
    wchar_t buf[1024];
    va_list ap;
    if (!s_verbose) return;
    va_start(ap, fmt);
    _vsnwprintf(buf, 1023, fmt, ap);
    buf[1023] = 0;
    va_end(ap);
    LogLine(buf);
}

void WctlLogAlways(const wchar_t* fmt, ...)
{
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 1023, fmt, ap);
    buf[1023] = 0;
    va_end(ap);
    LogLine(buf);
}

void WctlSetStatus(const wchar_t* fmt, ...)
{
    wchar_t buf[220];
    va_list ap;
    if (!g_shm) return;
    va_start(ap, fmt);
    _vsnwprintf(buf, 219, fmt, ap);
    buf[219] = 0;
    va_end(ap);
    EnterCriticalSection(&g_cs);
    lstrcpynW((wchar_t*)g_shm->status, buf, 200);
    LeaveCriticalSection(&g_cs);
}

/* ------------------------------------------------------------------ */
/* 工具                                                               */
/* ------------------------------------------------------------------ */
static UINT (WINAPI *pGetDpiForWindow)(HWND) = NULL;
static BOOL s_dpiResolved = FALSE;

UINT WctlDpiOf(HWND hwnd)
{
    if (!s_dpiResolved) {
        HMODULE u = GetModuleHandleW(L"user32.dll");
        if (u) pGetDpiForWindow = (UINT (WINAPI*)(HWND))GetProcAddress(u, "GetDpiForWindow");
        s_dpiResolved = TRUE;
    }
    if (pGetDpiForWindow && hwnd && IsWindow(hwnd)) {
        UINT d = pGetDpiForWindow(hwnd);
        if (d >= 48 && d <= 480) return d;
    }
    {
        HDC dc = GetDC(NULL);
        UINT d = 96;
        if (dc) { d = (UINT)GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(NULL, dc); }
        if (d < 48 || d > 480) d = 96;
        return d;
    }
}

int WctlScale(int logical, UINT dpi) { return MulDiv(logical, (int)dpi, 96); }

BOOL WctlRectEqualsRect(const RECT* a, const RECT* b, int tol)
{
    return (a->left   >= b->left   - tol && a->left   <= b->left   + tol &&
            a->top    >= b->top    - tol && a->top    <= b->top    + tol &&
            a->right  >= b->right  - tol && a->right  <= b->right  + tol &&
            a->bottom >= b->bottom - tol && a->bottom <= b->bottom + tol);
}

BOOL WctlRectCoversMonitor(const RECT* r, const RECT* mon, int tol)
{
    return (r->left   <= mon->left   + tol &&
            r->top    <= mon->top    + tol &&
            r->right  >= mon->right  - tol &&
            r->bottom >= mon->bottom - tol);
}

BOOL WctlRectEmptyOrTiny(const RECT* r)
{
    return (r->right - r->left) < 40 || (r->bottom - r->top) < 30;
}

typedef struct MonSearch {
    int         index;
    int         found;
    MONITORINFO mi;
} MonSearch;

static BOOL CALLBACK MonEnumProc(HMONITOR hm, HDC dc, LPRECT rc, LPARAM lp)
{
    MonSearch* ms = (MonSearch*)lp;
    (void)dc; (void)rc;
    if (ms->index == 0) {
        ms->mi.cbSize = sizeof(MONITORINFO);
        GetMonitorInfoW(hm, &ms->mi);
        ms->found = 1;
        return FALSE;
    }
    ms->index--;
    return TRUE;
}

BOOL WctlGetMonitorInfoFor(HWND hwnd, int monIdx, MONITORINFO* out)
{
    if (!out) return FALSE;
    out->cbSize = sizeof(MONITORINFO);
    if (monIdx >= 0) {
        MonSearch ms;
        memset(&ms, 0, sizeof(ms));
        ms.index = monIdx;
        EnumDisplayMonitors(NULL, NULL, MonEnumProc, (LPARAM)&ms);
        if (ms.found) { *out = ms.mi; return TRUE; }
    }
    {
        HMONITOR hm = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (!hm) hm = MonitorFromWindow(NULL, MONITOR_DEFAULTTOPRIMARY);
        return GetMonitorInfoW(hm, out);
    }
}

BOOL WctlIsFullscreenLike(HWND hwnd, RECT* outRect)
{
    LONG_PTR style;
    RECT r;
    HMONITOR hm;
    MONITORINFO mi;

    if (!hwnd || !IsWindow(hwnd)) return FALSE;
    style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((style & WS_CAPTION) == WS_CAPTION) return FALSE;   /* 有标题栏就不算全屏 */
    if (!GetWindowRect(hwnd, &r)) return FALSE;
    if (WctlRectEmptyOrTiny(&r)) return FALSE;
    hm = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hm, &mi)) return FALSE;
    if (!WctlRectCoversMonitor(&r, &mi.rcMonitor, 2)) return FALSE;
    if (outRect) *outRect = r;
    return TRUE;
}

RECT WctlDefaultNormalRect(HWND hwnd)
{
    MONITORINFO mi;
    RECT r;
    int w, h;
    WctlGetMonitorInfoFor(hwnd, g_shm ? (int)g_shm->monitor : -1, &mi);
    w = (mi.rcWork.right - mi.rcWork.left) * 70 / 100;
    h = (mi.rcWork.bottom - mi.rcWork.top) * 70 / 100;
    if (w < 480) w = 480;
    if (h < 360) h = 360;
    r.left = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
    r.top  = mi.rcWork.top  + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
    r.right = r.left + w;
    r.bottom = r.top + h;
    return r;
}

const wchar_t* WctlExeName(void) { return s_exeName; }

/* ------------------------------------------------------------------ */
/* "内容不随窗口缩放"的目标 (远程桌面 / 屏幕广播 / 视频播放器 之类)      */
/* ------------------------------------------------------------------ */
/* 这类程序把远端画面按"固定分辨率"画进一个内容视图子窗口里: 我们缩小窗口,
 * 窗口本身变小了, 但画面仍旧 1:1 画在上面 => 只能看到左上角一部分。
 *
 * 我们能拿到的客观信号: 它的客户区里有一个"内容视图"子窗口(通常占满客户区,
 * 类名类似 AfxWnd80u / Internet Explorer_Server / 各家自绘类)。
 * 反过来, 完全自绘(没有任何子窗口)的目标(比如 testapp --lockdown)缩小窗口不会
 * 丢内容, 那种情况继续按老办法缩到工作区 70% —— 这样两类程序各得其所。
 *
 * 判定为"内容固定"时, 窗口化就不再缩小客户区(见 WctlAutoSize), 而是保持原始内容
 * 尺寸 + 补标题栏, 宁可窗口比屏幕大、让用户拖动查看, 也不裁掉内容。 */
static BOOL s_contentKept  = FALSE;   /* 我们是否因此保持了原始尺寸 */

BOOL WctlContentWasKept(void) { return s_contentKept; }

/* 在一个窗口的客户区里找"内容视图子窗口": 面积最大的、且宽高都覆盖客户区六成以上的
 * 可见子窗口(往下找几层: 内容视图常被套在容器窗口里)。找不到就返回 NULL。 */
static HWND FindContentChildRec(HWND parent, const RECT* cliScreen, int depth, LONG* bestArea)
{
    HWND child, best = NULL;
    int guard = 0;

    child = GetWindow(parent, GW_CHILD);
    for (; child && guard < 200; guard++) {
        RECT r;
        LONG area;
        if (IsWindowVisible(child) && GetWindowRect(child, &r)) {
            if ((r.right - r.left) >= (cliScreen->right - cliScreen->left) * 6 / 10 &&
                (r.bottom - r.top) >= (cliScreen->bottom - cliScreen->top) * 6 / 10) {
                area = (r.right - r.left) * (r.bottom - r.top);
                if (area > *bestArea) { *bestArea = area; best = child; }
                if (depth < 3) {
                    LONG sub = 0;
                    HWND d = FindContentChildRec(child, cliScreen, depth + 1, &sub);
                    if (d && sub > *bestArea) { *bestArea = sub; best = d; }
                }
            }
        }
        child = GetWindow(child, GW_HWNDNEXT);
    }
    return best;
}

HWND WctlFindContentChild(HWND hwnd)
{
    RECT cr, cliScreen;
    POINT p;
    LONG best = 0;

    if (!hwnd || !IsWindow(hwnd)) return NULL;
    if (!GetClientRect(hwnd, &cr)) return NULL;
    if (cr.right < 200 || cr.bottom < 150) return NULL;   /* 小窗口不掺和 */
    p.x = 0; p.y = 0;
    if (!ClientToScreen(hwnd, &p)) return NULL;
    cliScreen.left   = p.x;
    cliScreen.top    = p.y;
    cliScreen.right  = p.x + cr.right;
    cliScreen.bottom = p.y + cr.bottom;
    return FindContentChildRec(hwnd, &cliScreen, 0, &best);
}

BOOL WctlContentLooksFixed(HWND hwnd, HWND* outChild)
{
    RECT cr, wr;
    HWND best;
    MONITORINFO mi;

    if (outChild) *outChild = NULL;
    if (!hwnd || !IsWindow(hwnd)) return FALSE;
    if (!GetClientRect(hwnd, &cr)) return FALSE;
    if (!GetWindowRect(hwnd, &wr)) return FALSE;

    /* 只对"铺满显示器的无边框窗口 / 客户区几乎占满工作区的大窗口"动手: 只有这种
       目标被缩小才会真的裁掉内容。用户自己调过尺寸的普通窗口保持原样。 */
    WctlGetMonitorInfoFor(hwnd, g_shm ? (int)g_shm->monitor : -1, &mi);
    if (!WctlRectCoversMonitor(&wr, &mi.rcMonitor, 4)) {
        LONG ww = mi.rcWork.right - mi.rcWork.left;
        LONG wh = mi.rcWork.bottom - mi.rcWork.top;
        if (ww <= 0 || wh <= 0) return FALSE;
        if ((LONGLONG)cr.right * cr.bottom < (LONGLONG)ww * wh * 9 / 10) return FALSE;
    }

    best = WctlFindContentChild(hwnd);
    if (outChild) *outChild = best;
    if (!best) {
        /* 没有子窗口: 自绘程序, 缩小窗口不会因为"固定画面"而丢内容, 维持原行为 */
        WctlLog(L"没有内容视图子窗口, 判定为可安全缩小");
        return FALSE;
    }
    {
        wchar_t cls[64];
        RECT r;
        cls[0] = 0;
        GetClassNameW(best, cls, 63);
        GetWindowRect(best, &r);
        WctlLog(L"检测到内容视图子窗口 %s (%dx%d), 判定为\"内容不随窗口缩放\"",
                cls, (int)(r.right - r.left), (int)(r.bottom - r.top));
    }
    return TRUE;
}

LONG WctlFlags(void)
{
    if (!g_shm) return WCTL_DEFAULT_FLAGS;
    return g_shm->flags;
}

void WctlForceApply(void)
{
    if (g_tickThreadId) PostThreadMessageW(g_tickThreadId, WCTL_WM_APPLY, 0, 0);
}

/* 由目标窗口线程调用: 限流后请求一次应用(避免拖拽窗口时刷屏) */
void WctlPostApply(void)
{
    static volatile LONG last = 0;
    DWORD now = GetTickCount();
    LONG prev = InterlockedExchange((LONG volatile*)&last, (LONG)now);
    if ((LONG)now - prev < 25) return;
    WctlForceApply();
}

/* ------------------------------------------------------------------ */
/* IPC                                                                */
/* ------------------------------------------------------------------ */
BOOL WctlIpcOpen(void)
{
    wchar_t name[128];
    DWORD pid = GetCurrentProcessId();

    _snwprintf(name, 127, WCTL_SHM_FMT, pid);
    name[127] = 0;
    g_shmMap = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                                  sizeof(WctlShared), name);
    if (!g_shmMap) return FALSE;
    g_shm = (WctlShared*)MapViewOfFile(g_shmMap, FILE_MAP_ALL_ACCESS, 0, 0,
                                       sizeof(WctlShared));
    if (g_shm && g_shm->magic != WCTL_SHM_MAGIC) {
        /* 首次创建: 初始化默认值 */
        memset((void*)g_shm, 0, sizeof(WctlShared));
        g_shm->magic      = WCTL_SHM_MAGIC;
        g_shm->version    = WCTL_SHM_VERSION;
        g_shm->structSize = sizeof(WctlShared);
        g_shm->flags      = WCTL_DEFAULT_FLAGS;
        g_shm->monitor    = -1;
        g_shm->panelHeight   = 30;
        /* 默认"鼠标不在上面时完全透明": 面板平时看不见, 鼠标移到它那一小块才显形。
           想要常显的半透明条, 用 --panel-alpha=110 (或 GUI 里的开关)。 */
        g_shm->panelAlpha    = 0;
        g_shm->panelAlphaHot = 255;
        g_shm->panelPos      = WCTL_PANELPOS_CENTER;   /* 只占需要的那点宽度, 居中 */
        g_shm->panelVPos     = WCTL_PANELVPOS_TOP;
        g_shm->panelWidth    = 0;                      /* 0 = 按按钮内容自适应宽度 */
        g_shm->panelX        = 0;
        g_shm->panelY        = 0;
        g_shm->hotkeyTop  = 0x54;   /* T */
        g_shm->hotkeyFull = 0x46;   /* F */
        g_shm->hotkeyLock = 0x4C;   /* L */
        g_shm->hotkeyPanel= 0x48;   /* H */
        g_shm->pid        = (LONG)pid;
        g_shm->lockValid  = 0;
    } else if (g_shm && g_shm->version != WCTL_SHM_VERSION) {
        /* 注入器与 DLL 来自不同版本: 结构体布局可能不一致, 说清楚免得排查半天 */
        WctlLogAlways(L"wctl: 共享内存版本不一致 (注入器=%ld, DLL=%d), 请把 winctl.exe 与 "
                      L"winctl_hook.dll 一起重新构建",
                      (long)g_shm->version, (int)WCTL_SHM_VERSION);
    }
    _snwprintf(name, 127, WCTL_EVT_FMT, pid);
    name[127] = 0;
    g_tickEvent = CreateEventW(NULL, FALSE, FALSE, name);
    return g_shm != NULL;
}

void WctlIpcPoll(void)
{
    LONG want, ack;
    if (!g_shm) return;

    /* 位置锁参数: 注入器可以直接指定 */
    if (g_shm->lockValid == 1 && g_shm->cmdSeq != g_shm->ackSeq) {
        g_shm->lockValid = 2;   /* 2 = 已受理 */
    }

    want = g_shm->cmdSeq;
    ack  = g_shm->ackSeq;
    if (want != ack) {
        WctlLog(L"IPC cmd=%d seq=%d", (int)g_shm->lastCmd, (int)want);
        InterlockedExchange((LONG volatile*)&g_shm->ackSeq, want);
        s_verbose = (g_shm->flags & WCTL_F_VERBOSE) ? 1 : 0;
        WctlForceApply();
    }
}

/* ------------------------------------------------------------------ */
/* 窗口登记表                                                          */
/* ------------------------------------------------------------------ */
WndCtx* WctlFindCtx(HWND hwnd)
{
    WndCtx* r = NULL;
    EnterCriticalSection(&g_cs);
    for (WndCtx* c = g_ctxList; c; c = c->next) {
        if (c->hwnd == hwnd && !c->dead) { r = c; break; }
    }
    LeaveCriticalSection(&g_cs);
    return r;
}

static int SnapshotCtx(WndCtx** out, int maxn)
{
    int n = 0;
    EnterCriticalSection(&g_cs);
    for (WndCtx* c = g_ctxList; c && n < maxn; c = c->next) {
        if (!c->dead) out[n++] = c;
    }
    LeaveCriticalSection(&g_cs);
    return n;
}

int WctlCtxCount(void)
{
    int n = 0;
    EnterCriticalSection(&g_cs);
    for (WndCtx* c = g_ctxList; c; c = c->next) if (!c->dead) n++;
    LeaveCriticalSection(&g_cs);
    return n;
}

void WctlSyncFromWindow(WndCtx* c)
{
    LONG_PTR style, ex;
    RECT r;

    if (!c || c->dead || !IsWindow(c->hwnd)) return;
    /* 重映射期间源窗口被我们摆在屏幕外, 那个几何不是"正常状态", 不能记进 normalRect */
    if (WctlRemapActive() && c->hwnd == WctlRemapSource()) return;
    if (IsIconic(c->hwnd) || IsZoomed(c->hwnd)) return;
    if (c->fullForced) return;                       /* 我们自己造成的全屏不算正常状态 */
    if (WctlIsFullscreenLike(c->hwnd, NULL)) return; /* 目标处于全屏, 保留旧记录 */
    if (!GetWindowRect(c->hwnd, &r)) return;
    if (WctlRectEmptyOrTiny(&r)) return;
    style = GetWindowLongPtrW(c->hwnd, GWL_STYLE);
    ex    = GetWindowLongPtrW(c->hwnd, GWL_EXSTYLE);
    c->normalRect    = r;
    c->normalStyle   = style;
    c->normalExStyle = ex;
    c->haveNormal    = TRUE;
}

static WndCtx* AddCtx(HWND hwnd)
{
    WndCtx* c = (WndCtx*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(WndCtx));
    if (!c) return NULL;
    c->hwnd = hwnd;
    c->orig = (WNDPROC)GetWindowLongPtrW(hwnd, GWLP_WNDPROC);

    EnterCriticalSection(&g_cs);
    c->next = g_ctxList;
    g_ctxList = c;
    LeaveCriticalSection(&g_cs);

    SetLastError(0);
    if (!SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)WctlSubclassProc)) {
        DWORD err = GetLastError();
        if (err != 0) {
            WctlLog(L"subclass 失败 hwnd=%08X err=%lu", (unsigned)(ULONG_PTR)hwnd, err);
            EnterCriticalSection(&g_cs);
            if (g_ctxList == c) g_ctxList = c->next;
            else { for (WndCtx* p = g_ctxList; p; p = p->next) if (p->next == c) { p->next = c->next; break; } }
            LeaveCriticalSection(&g_cs);
            HeapFree(GetProcessHeap(), 0, c);
            return NULL;
        }
    }
    WctlSyncFromWindow(c);
    /* 趁这个窗口还是"它自己的样子"(我们还没改过它的样式/尺寸)先把"内容是否随窗口缩放"
       判掉: 远程桌面/屏幕广播类程序窗口化后只会裁掉画面, 后面 WctlAutoSize / 内容缩放
       都要靠这个结论。内容尺寸以"内容视图子窗口的客户区"为准 —— 目标程序就是按这个
       尺寸把远端画面画出来的。 */
    if (!c->contentFixed) {
        HWND child = NULL;
        if (WctlContentLooksFixed(hwnd, &child)) {
            RECT cr;
            c->contentFixed = TRUE;
            c->contentChild = child;
            if (child && GetClientRect(child, &cr) && cr.right > 0 && cr.bottom > 0) {
                c->contentW = cr.right;
                c->contentH = cr.bottom;
            } else if (GetClientRect(hwnd, &cr)) {
                c->contentW = cr.right;
                c->contentH = cr.bottom;
            }
            WctlLog(L"记下内容尺寸 %ldx%ld (子窗口 %08X, hwnd=%08X)",
                    (long)c->contentW, (long)c->contentH,
                    (unsigned)(ULONG_PTR)c->contentChild, (unsigned)(ULONG_PTR)hwnd);
        }
    }
    WctlLog(L"接管窗口 hwnd=%08X cls 样式=%08X", (unsigned)(ULONG_PTR)hwnd,
            (unsigned)c->normalStyle);
    return c;
}

void WctlMarkDead(HWND hwnd)
{
    EnterCriticalSection(&g_cs);
    for (WndCtx* c = g_ctxList; c; c = c->next) {
        if (c->hwnd == hwnd) {
            if (c->lockValid || c->lockSizeValid) {
                POINT p;
                p.x = c->lockValid ? c->lockPos.x : 0;
                p.y = c->lockValid ? c->lockPos.y : 0;
                GravePut(hwnd, &p, c->lockValid, c->lockW, c->lockH, c->lockSizeValid);
            }
            c->dead = TRUE;
            break;
        }
    }
    LeaveCriticalSection(&g_cs);
    if (g_primary == hwnd) g_primary = NULL;
    /* 窗口没了: 内容缩放立刻收工(它记着 WndCtx 指针, 不能等主循环来收) */
    WctlScaleEndByHwnd(hwnd);
}

void WctlReapDead(void)
{
    WndCtx* kill = NULL;
    EnterCriticalSection(&g_cs);
    WndCtx** pp = &g_ctxList;
    while (*pp) {
        WndCtx* c = *pp;
        if (c->dead) {
            *pp = c->next;
            c->next = kill;
            kill = c;
        } else {
            pp = &c->next;
        }
    }
    LeaveCriticalSection(&g_cs);
    while (kill) {
        WndCtx* n = kill->next;
        WctlScaleEndByHwnd(kill->hwnd);   /* 兜底: 不能让 ScaleWin 记着已释放的 WndCtx */
        HeapFree(GetProcessHeap(), 0, kill);
        kill = n;
    }
}

static BOOL IsIgnoredClass(HWND hwnd)
{
    wchar_t cls[64];
    cls[0] = 0;
    GetClassNameW(hwnd, cls, 63);
    if (!cls[0]) return FALSE;
    if (lstrcmpiW(cls, L"Default IME") == 0) return TRUE;
    if (lstrcmpiW(cls, L"MSCTFIME UI") == 0) return TRUE;
    if (lstrcmpiW(cls, L"IME") == 0) return TRUE;
    if (lstrcmpiW(cls, L"ToolTip32") == 0) return TRUE;
    if (lstrcmpiW(cls, L"SysShadow") == 0) return TRUE;
    if (lstrcmpiW(cls, L"#32768") == 0) return TRUE;   /* 菜单窗口 */
    return FALSE;
}

static BOOL CALLBACK DiscoverProc(HWND hwnd, LPARAM lp)
{
    DWORD pid = 0;
    LONG_PTR ex;
    RECT r;

    (void)lp;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    if (GetParent(hwnd) != NULL) return TRUE;          /* 只看顶层窗口 */
    if (WctlIsPanel(hwnd)) return TRUE;
    if (WctlIsRemapViewer(hwnd)) return TRUE;
    if (WctlFindCtx(hwnd)) return TRUE;
    if (IsIgnoredClass(hwnd)) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (!GetWindowRect(hwnd, &r)) return TRUE;
    if (WctlRectEmptyOrTiny(&r)) return TRUE;
    ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_NOACTIVATE) return TRUE;            /* 输入法候选框之类 */
    AddCtx(hwnd);
    return TRUE;
}

void WctlDiscover(void) { EnumWindows(DiscoverProc, 0); }

void WctlUnsubclassAll(void)
{
    WndCtx* snap[WCTL_MAX_CTX];
    int n = SnapshotCtx(snap, WCTL_MAX_CTX);
    for (int i = 0; i < n; i++) {
        WndCtx* c = snap[i];
        if (IsWindow(c->hwnd)) {
            LONG_PTR cur = GetWindowLongPtrW(c->hwnd, GWLP_WNDPROC);
            if (cur == (LONG_PTR)WctlSubclassProc && c->orig)
                SetWindowLongPtrW(c->hwnd, GWLP_WNDPROC, (LONG_PTR)c->orig);
        }
        EnterCriticalSection(&g_cs);
        c->dead = TRUE;
        LeaveCriticalSection(&g_cs);
    }
    WctlReapDead();
}

/* ------------------------------------------------------------------ */
/* 主窗口选择                                                          */
/* ------------------------------------------------------------------ */
void WctlPickPrimary(void)
{
    WndCtx* snap[WCTL_MAX_CTX];
    int n;
    HWND want = NULL;
    LONG best = 0;
    LONG curArea = 0;
    DWORD now = GetTickCount();
    static HWND lastHint = NULL;
    static DWORD lastPick = 0;
    HWND hint = g_shm ? WCTL_SHM_GET_HWND(g_shm) : NULL;
    BOOL hintChanged = (hint != lastHint);
    BOOL needPick = FALSE;

    lastHint = hint;
    if (hintChanged) needPick = TRUE;
    if (!g_primary || !IsWindow(g_primary) || !WctlFindCtx(g_primary)) needPick = TRUE;
    /* 每秒复核一次: 目标常会先出个闪屏小窗, 再换成真正霸屏的那个窗口 */
    if (now - lastPick >= 1000) needPick = TRUE;
    if (!needPick) return;
    lastPick = now;

    n = SnapshotCtx(snap, WCTL_MAX_CTX);
    if (hint && IsWindow(hint) && WctlFindCtx(hint) && IsWindowVisible(hint)) want = hint;
    if (!want) {
        for (int i = 0; i < n; i++) {
            WndCtx* c = snap[i];
            RECT r;
            LONG area;
            if (!IsWindowVisible(c->hwnd)) continue;
            if (IsIconic(c->hwnd)) continue;
            if (!GetWindowRect(c->hwnd, &r)) continue;
            area = (r.right - r.left) * (r.bottom - r.top);
            if (area > best) { best = area; want = c->hwnd; }
        }
    }
    if (g_primary && IsWindow(g_primary)) {
        RECT r;
        if (IsWindowVisible(g_primary) && !IsIconic(g_primary) && GetWindowRect(g_primary, &r))
            curArea = (r.right - r.left) * (r.bottom - r.top);
    }
    /* 已有主窗口时, 只有"它不可见/被最小化"或"出现了明显更大的窗口"才切换 */
    if (g_primary && IsWindow(g_primary) && want && want != g_primary) {
        if (curArea > 0 && best <= curArea + curArea / 4) want = g_primary;
    }
    if ((!want) && g_primary && IsWindow(g_primary)) want = g_primary;

    if (want != g_primary) {
        for (int i = 0; i < n; i++) snap[i]->isPrimary = FALSE;
        if (want) {
            WndCtx* c = WctlFindCtx(want);
            if (c) c->isPrimary = TRUE;
        }
        g_primary = want;
        /* 面板的 owner 变了: 不在这里改, 交给 WctlPanelTick —— 被拥有窗口只有在
           "owner 已经处于最终置顶状态"时才创建, 系统才会把它放进正确的层级
           (见 panel.cpp 里关于面板重建的说明)。 */
        if (g_panel && IsWindow(g_panel))
            WctlLog(L"主窗口变更 -> 面板将按新 owner 重建 hwnd=%08X", (unsigned)(ULONG_PTR)want);
        WctlLog(L"主窗口 = %08X (面积 %ld)", (unsigned)(ULONG_PTR)want, best);
    }
}

/* ------------------------------------------------------------------ */
/* 功能①: 置顶                                                        */
/* ------------------------------------------------------------------ */

/* 我们自己发起的 Z 序调整: 期间置位标记, 让窗口过程知道"这次请求不是目标程序发的",
 * 否则目标程序被吞掉的置顶请求规则会连我们自己的"取消置顶"一起吞掉。 */
void WctlSetZOrder(WndCtx* c, HWND insertAfter)
{
    if (!c || !IsWindow(c->hwnd)) return;
    InterlockedIncrement((LONG volatile*)&c->selfZOrder);
    if (insertAfter == HWND_NOTOPMOST) {
        /* 只靠 SetWindowPos 是不够的: 当"我们的面板自己也处于置顶带"时,
           系统会把这次请求解析成"插到某个置顶窗口下面", 窗口仍然留在置顶带里,
           于是取消置顶永远失败(实测会卡在已置顶状态)。直接把样式位清掉最可靠。 */
        LONG_PTR ex = GetWindowLongPtrW(c->hwnd, GWL_EXSTYLE);
        if (ex & WS_EX_TOPMOST)
            SetWindowLongPtrW(c->hwnd, GWL_EXSTYLE, ex & ~WS_EX_TOPMOST);
    }
    SetWindowPos(c->hwnd, insertAfter, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    InterlockedDecrement((LONG volatile*)&c->selfZOrder);
}

/* 一次性"跳至顶层": 借置顶带一瞬, 再立刻退出置顶。
 *
 * 不能直接用 SetWindowPos(HWND_TOP): 面板是目标的 owner 窗口, 系统在
 * WM_WINDOWPOSCHANGING 里会把 HWND_TOP 解析成"插到面板下面", 于是被
 * "置顶开关关闭时吞掉置顶请求"的规则当成置顶请求吃掉(实测完全没反应)。
 * 先 HWND_TOPMOST 再 HWND_NOTOPMOST 是系统认可的"抬到最前但不常驻置顶"
 * 做法, 最终它停在非置顶带的最上面, 之后照常被别的窗口压下去。
 * 两次请求都计入 selfZOrder, 免得被我们自己的拦截规则吞掉。 */
void WctlFrontPush(WndCtx* c)
{
    if (!c || !IsWindow(c->hwnd)) return;
    InterlockedIncrement((LONG volatile*)&c->selfZOrder);
    SetWindowPos(c->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    SetWindowPos(c->hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    InterlockedDecrement((LONG volatile*)&c->selfZOrder);

    /* 面板是目标的 owner 窗口, 上面那次"置顶->取消置顶"会打乱两者的相对 Z 序
       (实测面板会被压到目标后面, 直接看不见)。这里把面板重新放回目标正上方。 */
    if (g_panel && IsWindow(g_panel) &&
        GetWindowLongPtrW(g_panel, GWLP_HWNDPARENT) == (LONG_PTR)c->hwnd) {
        HWND above = GetWindow(c->hwnd, GW_HWNDPREV);
        SetWindowPos(g_panel, above ? above : HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
}

/* 供窗口过程在"事后纠正置顶"时调用, 计入统计并触发自适应快周期 */
void WctlNoteTopmostClear(void)
{
    InterlockedIncrement((LONG volatile*)&s_clearCount);
    InterlockedExchange((LONG volatile*)&s_lastClearAt, (LONG)GetTickCount());
}

static void ApplyTopmost(WndCtx* c, BOOL want)
{
    LONG_PTR ex;
    BOOL isTop;

    if (!IsWindow(c->hwnd)) return;
    if (IsIconic(c->hwnd) && !want) return;
    ex = GetWindowLongPtrW(c->hwnd, GWL_EXSTYLE);
    isTop = (ex & WS_EX_TOPMOST) != 0;
    if (want && !isTop) {
        WctlSetZOrder(c, HWND_TOPMOST);
        WctlLog(L"强制置顶 hwnd=%08X", (unsigned)(ULONG_PTR)c->hwnd);
    } else if (!want && isTop) {
        WctlSetZOrder(c, HWND_NOTOPMOST);
        c->blockTopmost++;
        InterlockedIncrement((LONG volatile*)&s_clearCount);
        InterlockedExchange((LONG volatile*)&s_lastClearAt, (LONG)GetTickCount());
        if (c->blockTopmost <= 5 || (c->blockTopmost % 40) == 0)
            WctlLog(L"取消置顶(吞掉置顶请求) #%ld hwnd=%08X", c->blockTopmost,
                    (unsigned)(ULONG_PTR)c->hwnd);
    }
}

/* ------------------------------------------------------------------ */
/* 功能②: 全屏 / 标题栏                                                */
/* ------------------------------------------------------------------ */
static void ClampRectToWork(const RECT* in, const RECT* work, RECT* out)
{
    int w = in->right - in->left;
    int h = in->bottom - in->top;
    int x = in->left;
    int y = in->top;
    if (w > work->right - work->left) w = work->right - work->left;
    if (h > work->bottom - work->top) h = work->bottom - work->top;
    if (w < 320) w = 320;
    if (h < 240) h = 240;
    if (x < work->left) x = work->left;
    if (y < work->top) y = work->top;
    if (x + w > work->right) x = work->right - w;
    if (y + h > work->bottom) y = work->bottom - h;
    out->left = x; out->top = y; out->right = x + w; out->bottom = y + h;
}

#define WCTL_CAPTION_BITS_LOCAL 1

static void ApplyFullscreen(WndCtx* c, BOOL want)
{
    HWND h = c->hwnd;
    LONG_PTR style, newStyle;
    RECT cur;
    BOOL hasCaption;
    BOOL frame;

    if (!h || !IsWindow(h)) return;
    style = GetWindowLongPtrW(h, GWL_STYLE);
    hasCaption = ((style & WS_CAPTION) == WS_CAPTION);
    if (!GetWindowRect(h, &cur)) return;

    if (want) {
        MONITORINFO mi;
        RECT target;
        BOOL needFrame = FALSE;

        if (!c->fullForced) {
            /* 进入强制全屏前先把"正常状态"留档 */
            if (!hasCaption || !WctlIsFullscreenLike(h, NULL)) {
                if (!IsIconic(h) && !IsZoomed(h) && !WctlRectEmptyOrTiny(&cur)) {
                    c->normalRect    = cur;
                    c->normalStyle   = style;
                    c->normalExStyle = GetWindowLongPtrW(h, GWL_EXSTYLE);
                    c->haveNormal    = TRUE;
                }
            }
            if (!c->haveNormal) {
                c->normalRect    = WctlDefaultNormalRect(h);
                c->normalStyle   = style | WCTL_CAPTION_BITS;
                c->normalExStyle = GetWindowLongPtrW(h, GWL_EXSTYLE);
                c->haveNormal    = TRUE;
            }
        }

        WctlGetMonitorInfoFor(h, g_shm ? (int)g_shm->monitor : -1, &mi);
        target = mi.rcMonitor;

        newStyle = style & ~(WS_CAPTION | WS_THICKFRAME);
        if (newStyle != style) {
            SetWindowLongPtrW(h, GWL_STYLE, newStyle);
            style = newStyle;
            needFrame = TRUE;
        }
        if (!WctlRectEqualsRect(&cur, &target, 0) || needFrame) {
            SetWindowPos(h, NULL, target.left, target.top,
                         target.right - target.left, target.bottom - target.top,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                         (needFrame ? SWP_FRAMECHANGED : 0));
            WctlLog(L"强制全屏 hwnd=%08X %d,%d,%d,%d", (unsigned)(ULONG_PTR)h,
                    (int)target.left, (int)target.top,
                    (int)(target.right - target.left), (int)(target.bottom - target.top));
        }
        c->fullForced = TRUE;
        c->captionAdded = FALSE;
        return;
    }

    /* ---- 取消全屏: 强制显示/创建标题栏 ---- */
    if (!c->fullForced && !WctlIsFullscreenLike(h, NULL) && hasCaption) return;

    if (IsZoomed(h)) {
        ShowWindow(h, SW_RESTORE);
        GetWindowRect(h, &cur);
        style = GetWindowLongPtrW(h, GWL_STYLE);
    }

    {
        MONITORINFO mi;
        RECT want2;
        BOOL hadCaption = ((style & WS_CAPTION) == WS_CAPTION);
        if (c->haveNormal) want2 = c->normalRect;
        else               want2 = WctlDefaultNormalRect(h);
        WctlGetMonitorInfoFor(h, g_shm ? (int)g_shm->monitor : -1, &mi);
        ClampRectToWork(&want2, &mi.rcWork, &want2);

        newStyle = c->haveNormal ? c->normalStyle : style;
        if ((newStyle & WS_CAPTION) != WS_CAPTION) {
            newStyle |= WCTL_CAPTION_BITS;
        }
        frame = (newStyle != style);
        if (frame) {
            SetWindowLongPtrW(h, GWL_STYLE, newStyle);
            c->captionAdded = !hadCaption && ((newStyle & WS_CAPTION) == WS_CAPTION);
            WctlLog(L"补齐标题栏 hwnd=%08X 样式=%08X", (unsigned)(ULONG_PTR)h,
                    (unsigned)newStyle);
        }
        SetWindowPos(h, NULL, want2.left, want2.top,
                     want2.right - want2.left, want2.bottom - want2.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        WctlLog(L"退出全屏 hwnd=%08X %d,%d,%d,%d 标题栏=%d", (unsigned)(ULONG_PTR)h,
                (int)want2.left, (int)want2.top,
                (int)(want2.right - want2.left), (int)(want2.bottom - want2.top),
                (int)c->captionAdded);
    }
    c->fullForced = FALSE;
}

/* ------------------------------------------------------------------ */
/* 功能③: 移动锁 / 大小锁 / 窗口状态锁                                 */
/* ------------------------------------------------------------------ */
/* 把"被最大化/最小化"的窗口按回普通状态(锁定窗口状态时不许这两种状态)。
 *
 * 分工:
 *   - 用户点标题栏/系统菜单的最小化、最大化, 以及目标程序自己发的
 *     WM_SYSCOMMAND, 在窗口过程里就被吞掉了(见 wndproc.cpp), 完全不闪;
 *   - 别的进程(服务端看门狗)直接 ShowWindow 的拦不到, 靠这里对账按回来,
 *     最坏有 15~50ms 的可见时间(和抢置顶同样处理)。
 *   - 锁上那一刻本来就处于最大化/最小化的, 尊重当时的状态, 不去"纠正"。 */
static void ApplyLockWindowState(WndCtx* c)
{
    LONG f = WctlFlags();
    BOOL wantMax, wantMin;
    LONG epoch = s_lockEpoch;
    HWND h;

    if (!c || !IsWindow(c->hwnd)) return;
    h = c->hwnd;

    /* 重新锁定(勾上/改开关)的那一刻, 先记下窗口当时是不是最大化/最小化 */
    if (c->lockEpoch != epoch) {
        c->zoomWasFrozen = IsZoomed(h) ? TRUE : FALSE;
        c->iconWasFrozen = IsIconic(h) ? TRUE : FALSE;
    }

    wantMax = ((f & WCTL_F_FREEZE) != 0) || ((f & WCTL_F_LOCKSIZE) != 0);
    wantMin = (f & WCTL_F_FREEZE) != 0;          /* 最小化只归"锁定窗口状态"管 */

    if (wantMax && !c->zoomWasFrozen && IsZoomed(h)) {
        ShowWindow(h, SW_RESTORE);
        c->blockState++;
        WctlLog(L"吞掉最大化(%s) #%ld hwnd=%08X",
                (f & WCTL_F_FREEZE) ? L"窗口状态已锁" : L"大小已锁",
                c->blockState, (unsigned)(ULONG_PTR)h);
    }
    if (wantMin && !c->iconWasFrozen && IsIconic(h)) {
        ShowWindow(h, SW_RESTORE);
        c->blockState++;
        WctlLog(L"吞掉最小化(窗口状态已锁) #%ld hwnd=%08X", c->blockState,
                (unsigned)(ULONG_PTR)h);
    }
}

static void ApplyLockGeom(WndCtx* c, BOOL lockPos, BOOL lockSize)
{
    RECT r;
    LONG epoch;
    BOOL fresh;

    if (!lockPos && !lockSize) {
        c->lockValid = FALSE;
        c->lockSizeValid = FALSE;
        c->zoomWasFrozen = FALSE;
        c->iconWasFrozen = FALSE;
        return;
    }
    if (!IsWindow(c->hwnd)) return;
    if (c->userMoving && (WctlFlags() & WCTL_F_USERMOVE)) {
        /* 用户正在拖动: 放开限制, 拖完下一个 tick 会以新位置/尺寸为锁定点 */
        c->lockValid = FALSE;
        c->lockSizeValid = FALSE;
        return;
    }
    if (IsIconic(c->hwnd) || IsZoomed(c->hwnd)) return;   /* 状态锁已先处理过 */
    if (!GetWindowRect(c->hwnd, &r)) return;
    if (WctlRectEmptyOrTiny(&r)) return;

    epoch = s_lockEpoch;
    fresh = (c->lockEpoch == 0);   /* 刚被接管的窗口 */

    /* 只有"类名与刚被销毁的那个窗口相同"的新窗口(即重建)才继承旧几何;
       多窗口程序里另一个窗口按它自己的当前几何锁定, 不会被叠到同一个位置。 */
    {
        POINT mp;
        LONG  mw = 0, mh = 0;
        BOOL  memoTaken = FALSE;
        if (fresh) memoTaken = GraveTake(c->hwnd, &mp, &mw, &mh);

        if (lockPos && (!c->lockValid || c->lockEpoch != epoch)) {
            if (memoTaken) {
                c->lockPos = mp;
            } else if (g_shm && g_shm->lockValid == 2) {
                c->lockPos.x = g_shm->lockX;
                c->lockPos.y = g_shm->lockY;
                g_shm->lockValid = 0;
            } else {
                c->lockPos.x = r.left;
                c->lockPos.y = r.top;
            }
            c->lockValid = TRUE;
            WctlLog(L"锁定位置 hwnd=%08X -> %d,%d%s", (unsigned)(ULONG_PTR)c->hwnd,
                    (int)c->lockPos.x, (int)c->lockPos.y, memoTaken ? L" (沿用重建前)" : L"");
        }
        if (lockSize && (!c->lockSizeValid || c->lockEpoch != epoch)) {
            if (memoTaken && mw > 0 && mh > 0) {
                c->lockW = mw;
                c->lockH = mh;
            } else {
                c->lockW = r.right - r.left;
                c->lockH = r.bottom - r.top;
            }
            c->lockSizeValid = TRUE;
            WctlLog(L"锁定大小 hwnd=%08X -> %ldx%ld", (unsigned)(ULONG_PTR)c->hwnd,
                    (long)c->lockW, (long)c->lockH);
        }
    }
    c->lockEpoch = epoch;

    {
        int nx = lockPos ? c->lockPos.x : r.left;
        int ny = lockPos ? c->lockPos.y : r.top;
        int nw = lockSize ? c->lockW : (r.right - r.left);
        int nh = lockSize ? c->lockH : (r.bottom - r.top);
        if (nx != r.left || ny != r.top || nw != r.right - r.left || nh != r.bottom - r.top) {
            SetWindowPos(c->hwnd, NULL, nx, ny, nw, nh,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            c->blockMove++;
            WctlLog(L"吞掉位置/大小改动 hwnd=%08X %d,%d,%dx%d -> %d,%d,%dx%d",
                    (unsigned)(ULONG_PTR)c->hwnd, (int)r.left, (int)r.top,
                    (int)(r.right - r.left), (int)(r.bottom - r.top), nx, ny, nw, nh);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 锁定窗口状态: 勾选那一刻把"置顶/全屏"固化成期望值, 并锁住位置+大小     */
/* ------------------------------------------------------------------ */
static void HandleFreeze(void)
{
    BOOL now;
    if (!g_shm) return;
    now = (g_shm->flags & WCTL_F_FREEZE) != 0;
    if (now == s_freezeOn) return;
    s_freezeOn = now;
    if (now) {
        HWND p = g_primary;
        LONG nf = g_shm->flags;
        nf |= (WCTL_F_LOCKMOVE | WCTL_F_LOCKSIZE);
        if (p && IsWindow(p)) {
            LONG_PTR ex = GetWindowLongPtrW(p, GWL_EXSTYLE);
            if (ex & WS_EX_TOPMOST) nf |= WCTL_F_TOPMOST; else nf &= ~WCTL_F_TOPMOST;
            if (WctlIsFullscreenLike(p, NULL)) nf |= WCTL_F_FULLSCREEN;
            else nf &= ~WCTL_F_FULLSCREEN;
        } else {
            nf &= ~(WCTL_F_TOPMOST | WCTL_F_FULLSCREEN);
        }
        InterlockedExchange((LONG volatile*)&g_shm->flags, nf);
        InterlockedIncrement((LONG volatile*)&s_lockEpoch);   /* 以当前几何为新的锁定点 */
        /* 记下"锁上那一刻"窗口是不是正最大化/最小化: 是的话尊重它, 不当成待纠正的错 */
        if (p && IsWindow(p)) {
            WndCtx* c = WctlFindCtx(p);
            if (c) {
                c->zoomWasFrozen = IsZoomed(p) ? TRUE : FALSE;
                c->iconWasFrozen = IsIconic(p) ? TRUE : FALSE;
            }
        }
        WctlLogAlways(L"wctl: 已锁定窗口状态 (置顶=%d 全屏=%d 位置+大小已锁; "
                      L"最大化/最小化/全屏/移动/缩放 全部禁止, 窗口重建后继续)",
                      (nf & WCTL_F_TOPMOST) ? 1 : 0, (nf & WCTL_F_FULLSCREEN) ? 1 : 0);
    } else {
        /* 解锁时把它"强行打开"的位置锁/大小锁一并放开, 否则窗口还是动不了,
           用户会以为是解锁没生效(想单独锁位置/大小, 之后用 --lockmove/--locksize 再开)。 */
        LONG nf = g_shm->flags;
        nf &= ~(WCTL_F_LOCKMOVE | WCTL_F_LOCKSIZE);
        InterlockedExchange((LONG volatile*)&g_shm->flags, nf);
        InterlockedIncrement((LONG volatile*)&s_lockEpoch);
        WctlLogAlways(L"wctl: 已解除窗口状态锁定 (位置/大小锁一并放开; "
                      L"最大化/最小化/全屏/移动/缩放 恢复可用)");
    }
}

/* ------------------------------------------------------------------ */
/* 统一应用 + 状态汇报                                                 */
/* ------------------------------------------------------------------ */
void WctlApplyAll(void)
{
    WndCtx* snap[WCTL_MAX_CTX];
    int n = SnapshotCtx(snap, WCTL_MAX_CTX);
    LONG f = WctlFlags();
    BOOL wantTop = (f & WCTL_F_TOPMOST) != 0;
    BOOL allWin  = (f & WCTL_F_ALLWINDOWS) != 0;
    BOOL wantRemap = (f & WCTL_F_REMAP) != 0;

    if (!wantRemap) WctlRemapEnd();   /* 开关已经关掉: 立刻把源窗口搬回屏幕 */

    for (int i = 0; i < n; i++) {
        WndCtx* c = snap[i];
        if (!IsWindow(c->hwnd)) continue;
        if (c->isPrimary) {
            if (wantRemap) {
                /* 缩放规则被切换过: 重建重映射(几何与查看器都按新规则来) */
                if (WctlRemapActive() && WctlRemapScaleMode() != g_shm->remapScale)
                    WctlRemapEnd();
                /* 显示重映射接管几何: 源窗口以原始内容尺寸移到屏幕外, 由查看器显示。
                   全屏/位置/大小锁在这期间必须让路, 否则会把源窗口又拽回屏幕上。 */
                WctlScaleEnd(c);          /* 重映射是第二种实现, 与内容缩放互斥 */
                WctlRemapSync(c);
                if (WctlRemapActive()) {
                    /* 置顶/全屏在重映射期间作用到查看器上 */
                    WctlRemapApplyViewer(wantTop, (f & WCTL_F_FULLSCREEN) != 0);
                }
            } else {
                ApplyTopmost(c, wantTop);
                ApplyFullscreen(c, (f & WCTL_F_FULLSCREEN) != 0);
                ApplyLockWindowState(c);   /* 状态锁: 先把"被最大化/最小化"按回来, 再锁几何 */
                ApplyLockGeom(c, (f & WCTL_F_LOCKMOVE) != 0, (f & WCTL_F_LOCKSIZE) != 0);
                /* 内容缩放: 让"内容视图子窗口 + 离屏绘制"始终处于就绪状态(窗口重建、
                   子窗口重建都会在这里补上); 关掉开关时立刻收工还原。 */
                WctlScaleSync(c);
                WctlSyncFromWindow(c);
            }
        } else if (allWin) {
            ApplyTopmost(c, wantTop);
            ApplyFullscreen(c, (f & WCTL_F_FULLSCREEN) != 0);
            ApplyLockWindowState(c);
            ApplyLockGeom(c, (f & WCTL_F_LOCKMOVE) != 0, (f & WCTL_F_LOCKSIZE) != 0);
            WctlScaleSync(c);
        } else if (!wantTop) {
            /* 非主窗口: 同样吞掉置顶请求 */
            LONG_PTR ex = GetWindowLongPtrW(c->hwnd, GWL_EXSTYLE);
            if ((ex & WS_EX_TOPMOST) && !IsIconic(c->hwnd)) {
                WctlSetZOrder(c, HWND_NOTOPMOST);
                c->blockTopmost++;
                InterlockedIncrement((LONG volatile*)&s_clearCount);
                InterlockedExchange((LONG volatile*)&s_lastClearAt, (LONG)GetTickCount());
            }
        }
    }
}

void WctlRestoreWindow(WndCtx* c)
{
    HWND h;
    LONG_PTR style, ex;
    RECT r;
    MONITORINFO mi;

    if (!c || !IsWindow(c->hwnd)) return;
    if (c->isPrimary && WctlRemapActive()) WctlRemapEnd();
    h = c->hwnd;
    if (IsZoomed(h)) ShowWindow(h, SW_RESTORE);
    style = c->haveNormal ? c->normalStyle : GetWindowLongPtrW(h, GWL_STYLE);
    ex    = c->haveNormal ? c->normalExStyle : GetWindowLongPtrW(h, GWL_EXSTYLE);
    if ((style & WS_CAPTION) != WS_CAPTION) style |= WCTL_CAPTION_BITS;
    SetWindowLongPtrW(h, GWL_EXSTYLE, ex & ~WS_EX_TOPMOST);
    SetWindowLongPtrW(h, GWL_STYLE, style);
    r = c->haveNormal ? c->normalRect : WctlDefaultNormalRect(h);
    WctlGetMonitorInfoFor(h, -1, &mi);
    /* 这个窗口的内容是"不随窗口缩放"的, 而且我们刚才还缩小过它: 卸载后目标会立刻恢复
       按原分辨率 1:1 绘制, 那个小窗口里就只能看到画面左上角一块。所以这里还原成
       "完整显示内容"的尺寸(= 内容尺寸 + 标题栏/边框), 和 --keep-content 的做法一致。 */
    if ((WctlScaleIsOn(c) || c->scaleWasOn) && c->contentFixed &&
        c->contentW > 0 && c->contentH > 0 && !IsZoomed(h)) {
        RECT cr, wr;
        LONG dw, dh;
        if (GetClientRect(h, &cr) && GetWindowRect(h, &wr)) {
            dw = (wr.right - wr.left) - cr.right;
            dh = (wr.bottom - wr.top) - cr.bottom;
            if (dw < 0) dw = 0;
            if (dh < 0) dh = 0;
            r.left = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - (c->contentW + dw)) / 2;
            r.top  = mi.rcWork.top;
            if (r.left > mi.rcWork.right - 120) r.left = mi.rcWork.right - 120;
            if (r.left < mi.rcWork.left) r.left = mi.rcWork.left;
            r.right  = r.left + c->contentW + dw;
            r.bottom = r.top + c->contentH + dh;
        }
    }
    ClampRectToWork(&r, &mi.rcWork, &r);
    SetWindowPos(h, HWND_NOTOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    c->fullForced = FALSE;
    c->lockValid = FALSE;
    WctlLog(L"还原窗口 hwnd=%08X", (unsigned)(ULONG_PTR)h);
}

/* 按"窗口化自动尺寸"重摆一次: 工作区 70% 居中 + 保证有标题栏与可拖拽边框
 *
 * 三种情况:
 *   ① 内容缩放(--scale-content=on, 默认开): 窗口按内容比例缩小, 远端画面等比缩放显示,
 *      内容一个像素都不丢, 而且窗口不会比屏幕大;
 *   ② 保持内容尺寸(--scale-content=off + --keep-content=on): 窗口保持原始内容尺寸 + 补标题栏,
 *      宁可窗口比屏幕大, 让用户拖着看;
 *   ③ 强行缩小(两个都关): 缩到工作区 70%, 画面被裁掉一部分(显式要求时才这么做)。 */
void WctlAutoSize(void)
{
    HWND h = g_primary;
    WndCtx* c;
    RECT r;
    LONG_PTR style;
    MONITORINFO mi;
    BOOL keepContent;
    BOOL scaleContent;
    int  cw = 0, ch = 0;

    if (!h || !IsWindow(h)) return;
    c = WctlFindCtx(h);
    if (!c) return;
    /* "窗口化"要让窗口回到屏幕上, 与显示重映射冲突: 先停下重映射 */
    if (WctlFlags() & WCTL_F_REMAP) {
        if (g_shm) InterlockedAnd((LONG volatile*)&g_shm->flags, ~WCTL_F_REMAP);
        WctlRemapEnd();
    }
    if (IsZoomed(h)) ShowWindow(h, SW_RESTORE);

    WctlGetMonitorInfoFor(h, g_shm ? (int)g_shm->monitor : -1, &mi);

    /* ---- 先决定要不要"内容缩放" ---- */
    /* 优先用接管那一刻记下的结论(那时窗口还没被我们改过); 这个窗口是后来才接管的/
       当时不像霸屏窗口, 就当场再看一眼。 */
    if (!c->contentFixed && WctlContentLooksFixed(h, NULL)) {
        HWND child = WctlFindContentChild(h);
        c->contentFixed = TRUE;
        c->contentChild = child;
        if (child) {
            RECT ccr;
            if (GetClientRect(child, &ccr) && ccr.right > 0 && ccr.bottom > 0) {
                c->contentW = ccr.right;
                c->contentH = ccr.bottom;
            }
        }
    }
    scaleContent = ((WctlFlags() & WCTL_F_SCALECONTENT) != 0) && c->contentFixed;
    if (scaleContent) {
        /* 动手改窗口之前先把内容视图子窗口钉住 + 挂上绘制重定向:
           目标程序一旦按新客户区重排子窗口, 我们就拿不到完整内容了。 */
        WctlScaleSync(c);
        scaleContent = WctlScaleIsOn(c);
    }
    if (!scaleContent) WctlScaleEnd(c);
    s_contentKept = FALSE;

    keepContent = !scaleContent &&
                  (WctlFlags() & WCTL_F_KEEPCONTENT) != 0 &&
                  (c->contentFixed || WctlContentLooksFixed(h, NULL));
    s_contentKept = keepContent;

    style = GetWindowLongPtrW(h, GWL_STYLE);
    if ((style & WS_CAPTION) != WS_CAPTION) {
        SetWindowLongPtrW(h, GWL_STYLE, style | WCTL_CAPTION_BITS);
        c->captionAdded = TRUE;
        SetWindowPos(h, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }

    if (scaleContent) {
        /* 按内容比例缩小到工作区的 72% 以内(内容本来就小就不放大), 居中、贴工作区上边 */
        RECT cr;
        LONG dw, dh, wantW, wantH, availW, availH;
        LONGLONG kNum, kDen;
        cw = c->contentW > 0 ? c->contentW : 1920;
        ch = c->contentH > 0 ? c->contentH : 1080;
        GetClientRect(h, &cr);
        GetWindowRect(h, &r);
        dw = (r.right - r.left) - cr.right;   /* 标题栏+边框的厚度 */
        dh = (r.bottom - r.top) - cr.bottom;
        if (dw < 0) dw = 0;
        if (dh < 0) dh = 0;
        availW = (mi.rcWork.right - mi.rcWork.left) * 72 / 100;
        availH = (mi.rcWork.bottom - mi.rcWork.top) * 72 / 100;
        if (availW < 320) availW = 320;
        if (availH < 240) availH = 240;
        /* 取 min(availW/cw, availH/ch, 1), 用整数比较免得引入浮点 */
        if ((LONGLONG)availW * ch <= (LONGLONG)availH * cw) { kNum = availW; kDen = cw; }
        else                                               { kNum = availH; kDen = ch; }
        if (kNum > kDen) { kNum = 1; kDen = 1; }
        wantW = (LONG)((LONGLONG)cw * kNum / kDen);
        wantH = (LONG)((LONGLONG)ch * kNum / kDen);
        if (wantW < 320) wantW = 320;
        if (wantH < 240) wantH = 240;
        r.left = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - (wantW + dw)) / 2;
        r.top  = mi.rcWork.top + 8;
        if (r.left < mi.rcWork.left) r.left = mi.rcWork.left;
        r.right  = r.left + wantW + dw;
        r.bottom = r.top + wantH + dh;
        if (r.bottom > mi.rcWork.bottom) {          /* 太高就往上收一点 */
            r.top = mi.rcWork.bottom - (r.bottom - r.top);
            if (r.top < mi.rcWork.top) r.top = mi.rcWork.top;
            r.bottom = r.top + wantH + dh;
        }
    } else if (keepContent) {
        /* 客户区尺寸 = 它渲染内容用的尺寸(接管那一刻记下来的), 加标题栏只让窗口变大:
           客户区一个像素都不缩 -> 内容一个像素都不丢。 */
        RECT cr;
        int dw, dh;
        GetClientRect(h, &cr);              /* 当前客户区, 用来算标题栏+边框的厚度 */
        if (c->contentFixed && c->contentW > 0 && c->contentH > 0) {
            cw = c->contentW;
            ch = c->contentH;
        } else {
            cw = cr.right; ch = cr.bottom;
        }
        GetWindowRect(h, &r);
        dw = (r.right - r.left) - cr.right;
        dh = (r.bottom - r.top) - cr.bottom;
        if (dw < 0) dw = 0;
        if (dh < 0) dh = 0;
        r.left = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - (cw + dw)) / 2;
        r.top  = mi.rcWork.top;
        if (r.left > mi.rcWork.right - 120) r.left = mi.rcWork.right - 120;
        if (r.left < mi.rcWork.left) r.left = mi.rcWork.left;
        r.right  = r.left + cw + dw;
        r.bottom = r.top + ch + dh;
    } else {
        r = WctlDefaultNormalRect(h);
        ClampRectToWork(&r, &mi.rcWork, &r);
    }

    SetWindowPos(h, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    /* 系统会用"最大跟踪尺寸"把过大的窗口夹一下, 以实际结果为准记录 */
    if (GetWindowRect(h, &r)) { /* r 已是真实矩形 */ }
    c->normalRect    = r;
    c->normalStyle   = GetWindowLongPtrW(h, GWL_STYLE);
    c->normalExStyle = GetWindowLongPtrW(h, GWL_EXSTYLE);
    c->haveNormal    = TRUE;
    c->fullForced    = FALSE;
    if (scaleContent) {
        WctlLogAlways(L"wctl: 目标内容不随窗口缩放(远程桌面/屏幕广播类), 已启用内容缩放: "
                      L"窗口 %d,%d %dx%d, 内容 %dx%d 等比缩放显示(不会裁掉画面)",
                      (int)r.left, (int)r.top, (int)(r.right - r.left), (int)(r.bottom - r.top),
                      cw, ch);
    } else if (keepContent) {
        WctlLogAlways(L"wctl: 目标内容不随窗口缩放(远程桌面/屏幕广播类), 已保持原始内容尺寸 "
                      L"%dx%d(窗口 %dx%d, 可能超出屏幕, 可拖动查看); 想强行缩小用 --keep-content=off",
                      cw, ch, (int)(r.right - r.left), (int)(r.bottom - r.top));
    } else {
        WctlLogAlways(L"wctl: 已把目标窗口摆成窗口化尺寸 %d,%d %dx%d", (int)r.left, (int)r.top,
                      (int)(r.right - r.left), (int)(r.bottom - r.top));
    }
}

/* 强制关闭: 先礼貌地给进程内所有顶层窗口发 WM_CLOSE, 1.5 秒后仍未退出就强杀 */
static BOOL CALLBACK KillEnumProc(HWND hwnd, LPARAM lp)
{
    DWORD pid = 0;
    (void)lp;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    if (GetParent(hwnd) != NULL) return TRUE;
    if (WctlIsPanel(hwnd)) return TRUE;
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
    return TRUE;
}

void WctlRequestKill(void)
{
    if (InterlockedCompareExchange((LONG volatile*)&s_killActive, 1, 0) != 0) return;
    WctlLogAlways(L"wctl: 收到强制关闭请求, 先尝试 WM_CLOSE");
    WctlSetStatus(L"正在关闭目标进程 (先 WM_CLOSE, 超时后强杀)");
    EnumWindows(KillEnumProc, 0);
    InterlockedExchange((LONG volatile*)&s_killAt, (LONG)(GetTickCount() + 1500));
}

/* 焦点模拟: 目标询问"我是不是前台"时由 IAT 钩子回答"是";
   这里负责在开关打开/主窗口变化时补发一条"已激活"消息, 让消息驱动型程序也信。 */
static HWND s_focusPosted = NULL;
static BOOL s_focusOn = FALSE;

void WctlFocusTick(void)
{
    BOOL want;
    if (!g_shm) return;
    want = (g_shm->flags & WCTL_F_FOCUS) != 0 && g_primary && IsWindow(g_primary) &&
           !IsIconic(g_primary);
    if (!want) {
        if (s_focusOn) {
            s_focusOn = FALSE;
            s_focusPosted = NULL;
            WctlLogAlways(L"wctl: 焦点模拟已关闭");
        }
        return;
    }
    if (s_focusOn && s_focusPosted == g_primary) return;
    s_focusOn = TRUE;
    s_focusPosted = g_primary;

    PostMessageW(g_primary, WM_ACTIVATEAPP, TRUE, 0);
    PostMessageW(g_primary, WM_NCACTIVATE, TRUE, 0);
    PostMessageW(g_primary, WM_ACTIVATE, MAKELONG(WA_ACTIVE, 0), 0);
    PostMessageW(g_primary, WM_SETFOCUS, 0, 0);
    WctlLogAlways(L"wctl: 焦点模拟已启用 hwnd=%08X (前台查询将回答\"是\")",
                  (unsigned)(ULONG_PTR)g_primary);
}

static void UpdateActual(void)
{
    LONG act = 0;
    wchar_t buf[260];
    wchar_t st[16], sf[16], sm[16], sp[16], ss[16];
    HWND p = g_primary;

    if (p && IsWindow(p)) {
        LONG_PTR ex = GetWindowLongPtrW(p, GWL_EXSTYLE);
        if (ex & WS_EX_TOPMOST) act |= WCTL_F_TOPMOST;
        if (WctlIsFullscreenLike(p, NULL)) act |= WCTL_F_FULLSCREEN;
        {
            WndCtx* c = WctlFindCtx(p);
            if (c) {
                RECT wr;
                if (c->lockValid && GetWindowRect(p, &wr) &&
                    wr.left == c->lockPos.x && wr.top == c->lockPos.y)
                    act |= WCTL_F_LOCKMOVE;
                if (c->lockSizeValid && GetWindowRect(p, &wr) &&
                    (wr.right - wr.left) == c->lockW && (wr.bottom - wr.top) == c->lockH)
                    act |= WCTL_F_LOCKSIZE;
            }
        }
    }
    if (g_panel && IsWindowVisible(g_panel)) act |= WCTL_F_PANEL;
    if (g_shm && (g_shm->flags & WCTL_F_FOCUS)) act |= WCTL_F_FOCUS;
    if (WctlRemapActive()) act |= WCTL_F_REMAP;
    if (g_shm) g_shm->actual = act;

    lstrcpyW(st, (WctlFlags() & WCTL_F_TOPMOST)    ? L"开" : L"关");
    lstrcpyW(sf, (WctlFlags() & WCTL_F_FULLSCREEN) ? L"开" : L"关");
    if (!(WctlFlags() & WCTL_F_LOCKMOVE))          lstrcpyW(sm, L"松");
    else if (WctlFlags() & WCTL_F_USERMOVE)        lstrcpyW(sm, L"锁+可拖");
    else                                           lstrcpyW(sm, L"锁");
    lstrcpyW(ss, (WctlFlags() & WCTL_F_LOCKSIZE)   ? L"锁" : L"松");
    lstrcpyW(sp, (WctlFlags() & WCTL_F_PANEL)      ? L"显" : L"隐");

    {
        WndCtx* c = p ? WctlFindCtx(p) : NULL;
        _snwprintf(buf, 259, L"%s | 置顶:%s%s 全屏:%s%s 移动:%s 大小:%s 面板:%s%s%s%s%s"
                             L" | 拦截 顶%ld 移%ld 态%ld | hwnd=%08X%s",
                  s_exeName,
                  st, (act & WCTL_F_TOPMOST) ? L"(已置顶)" : L"",
                  sf, (act & WCTL_F_FULLSCREEN) ? L"(全屏)" : L"(有标题栏)",
                  sm, ss, sp,
                  (WctlFlags() & WCTL_F_FREEZE) ? L" [状态已锁定]" : L"",
                  (WctlFlags() & WCTL_F_ALLWINDOWS) ? L" [多窗口]" : L"",
                  (WctlFlags() & WCTL_F_FOCUS) ? L" [焦点模拟]" : L"",
                  WctlRemapActive() ? L" [显示重映射]" : L"",
                  c ? c->blockTopmost : 0L, c ? c->blockMove : 0L, c ? c->blockState : 0L,
                  (unsigned)(ULONG_PTR)p,
                  (c && WctlScaleIsOn(c)) ? L" [内容缩放:已启用]"
                                          : (s_contentKept ? L" [内容不缩放:已保持原始尺寸]" : L""));
        buf[259] = 0;
    }
    WctlSetStatus(L"%s", buf);
}

void WctlTick(void)
{
    static int disc = 0;
    DWORD now;

    if (g_detached) return;

    /* 强制关闭倒计时: WM_CLOSE 没人理就动手 */
    if (s_killActive && (LONG)GetTickCount() >= s_killAt) {
        WctlLogAlways(L"wctl: WM_CLOSE 超时(目标不接受关闭), 强制结束进程");
        TerminateProcess(GetCurrentProcess(), 0);
    }

    if (g_shm && g_shm->detach) {
        WctlShutdown();
        return;
    }
    if (g_shm && g_shm->killReq) {
        InterlockedExchange((LONG volatile*)&g_shm->killReq, 0);
        WctlRequestKill();
    }
    if (g_shm && g_shm->resizeReq) {
        InterlockedExchange((LONG volatile*)&g_shm->resizeReq, 0);
        WctlAutoSize();
    }

    WctlIpcPoll();
    if (++disc >= WCTL_DISCOVER_EVERY) { disc = 0; WctlDiscover(); }
    WctlReapDead();
    WctlPickPrimary();
    HandleFreeze();       /* 勾选"锁定窗口状态"时固化当前状态 */
    WctlApplyAll();       /* 先让目标进入"期望状态", 面板再按这个状态就位/重建(见 panel.cpp) */
    WctlFocusTick();      /* 焦点模拟: 开关/主窗口变化时补发激活消息 */
    WctlRemapTick();      /* 显示重映射: 校验源窗口与查看器 */
    WctlScaleTick();      /* 内容缩放: 补挂后加载的模块 / 拦不到绘制时回退 */
    WctlPanelTick();
    UpdateActual();

    /* 目标抢置顶很凶时(400ms 内又抢了一次)自动切到 15ms 快周期对账 */
    now = GetTickCount();
    if (s_lastClearAt && (now - (DWORD)s_lastClearAt) < 400)
        InterlockedExchange((LONG volatile*)&s_fastUntil, (LONG)(now + 3000));
    {
        DWORD want = ((LONG)now < (LONG)s_fastUntil) ? WCTL_TICK_FAST_MS : WCTL_TICK_MS;
        if (want != s_curInterval) {
            s_curInterval = want;
            SetTimer(NULL, 1, want, NULL);
            WctlLog(L"对账周期切换为 %lu ms", (unsigned long)want);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 卸载 / 还原                                                         */
/* ------------------------------------------------------------------ */
void WctlShutdown(void)
{
    WndCtx* snap[WCTL_MAX_CTX];
    int n;

    if (g_detached) return;
    WctlLogAlways(L"wctl: 收到卸载请求, 还原窗口并停止");
    WctlRemapShutdown();     /* 先停重映射: 把源窗口从屏幕外搬回来, 销毁查看器 */
    WctlScaleStopAll();      /* 先停内容缩放: 让目标程序恢复"自己画自己的" */
    n = SnapshotCtx(snap, WCTL_MAX_CTX);
    for (int i = 0; i < n; i++) WctlRestoreWindow(snap[i]);
    WctlUnsubclassAll();
    WctlPanelDestroy();
    g_primary = NULL;
    if (g_shm) {
        g_shm->ready = 0;
        g_shm->actual = 0;
        WctlSetStatus(L"已卸载, 窗口已还原");
    }
    g_detached = 1;
    g_stop = 1;
    if (g_tickThreadId) PostThreadMessageW(g_tickThreadId, WM_QUIT, 0, 0);
}

/* ------------------------------------------------------------------ */
/* 主循环                                                             */
/* ------------------------------------------------------------------ */
void WctlToggleFlag(LONG bit, LONG cmd)
{
    if (!g_shm) return;
    if (bit == WCTL_F_LOCKMOVE) InterlockedIncrement((LONG volatile*)&s_lockEpoch);
    InterlockedXor((LONG volatile*)&g_shm->flags, bit);
    InterlockedExchange((LONG volatile*)&g_shm->lastCmd, cmd);
    {
        LONG s = InterlockedIncrement((LONG volatile*)&g_shm->cmdSeq);
        InterlockedExchange((LONG volatile*)&g_shm->ackSeq, s);
    }
    WctlForceApply();
}

void WctlRun(void)
{
    MSG msg;
    DWORD pid = GetCurrentProcessId();
    wchar_t mainExe[MAX_PATH];

    g_tickThreadId = GetCurrentThreadId();
    WctlIpcOpen();
    s_verbose = (g_shm && (g_shm->flags & WCTL_F_VERBOSE)) ? 1 : 0;
    WctlScaleInit();      /* 内容缩放: 装好 GDI 绘制重定向的 IAT 挂钩(对没登记窗口无影响) */

    mainExe[0] = 0;
    GetModuleFileNameW(NULL, mainExe, MAX_PATH);
    {
        const wchar_t* b = mainExe;
        for (const wchar_t* q = mainExe; *q; q++) if (*q == L'\\' || *q == L'/') b = q + 1;
        lstrcpynW(s_exeName, b, 60);
    }
    if (g_shm) lstrcpynW((wchar_t*)g_shm->targetExe, mainExe, WCTL_MAX_PATH - 1);

    WctlLogAlways(L"wctl: 已注入 pid=%lu exe=%s", pid, s_exeName);

    /* 顺序要紧: 先发现窗口/选出主窗口/把目标摆成期望状态, 最后才建面板 ——
       面板(被拥有窗口)只有在创建那一刻 owner 已经处于最终置顶状态, 才能落对层级。 */
    WctlDiscover();
    WctlPickPrimary();
    WctlApplyAll();
    WctlPanelCreate();
    UpdateActual();

    if (g_shm) g_shm->ready = 1;
    SetTimer(NULL, 1, WCTL_TICK_MS, NULL);

    while (!g_stop && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.hwnd == NULL && msg.message == WM_TIMER)  { WctlTick(); continue; }
        if (msg.hwnd == NULL && msg.message == WM_HOTKEY) { WctlPanelHotkey(msg.wParam); continue; }
        if (msg.message == WCTL_WM_APPLY)                 { WctlTick(); continue; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    KillTimer(NULL, 1);
    WctlPanelDestroy();
    if (s_logFile != INVALID_HANDLE_VALUE) { CloseHandle(s_logFile); s_logFile = INVALID_HANDLE_VALUE; }
    WctlScaleShutdown();      /* 还原导入表 + 释放离屏 DC(此时目标程序已经恢复自己绘制) */
    WctlLogAlways(L"wctl: 主循环退出");
}
