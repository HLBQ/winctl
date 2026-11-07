/* 
    scale.cpp -- 类窗口的内容缩放
 */
#include "wctl_core.h"
#include <tlhelp32.h>
#include <wchar.h>

#define WCTL_SCALE_MAX   8      /* 同时最多缩放几个窗口 */
#define WCTL_DC_STACK    6      /* 每个窗口最多嵌套几层 DC */
#define WCTL_IAT_MAX     18     /* 挂钩的函数个数 */
#define WCTL_SLOT_MAX    1024   /* 记录被改过的导入表槽位(便于还原) */
#define WCTL_RETIRE_MAX  16     /* 已停止但暂不销毁的 GDI 对象(防"正在画"时被删) */

typedef struct ScaleWin {
    BOOL     used;
    HWND     child;              /* 内容视图子窗口 */
    WndCtx*  ctx;
    WNDPROC  orig;               /* 子窗口原来的窗口过程 */

    LONG     cw, ch;             /* 内容尺寸(= 子窗口客户区尺寸) */
    LONG     pinWinX, pinWinY;   /* 钉住的窗口位置(相对父客户区) */
    LONG     pinW, pinH;         /* 钉住的窗口大小 */
    LONG     cliOffX, cliOffY;   /* 客户区左上角相对窗口左上角的偏移(边框) */

    HDC      memDC;              /* 离屏 DC(内容尺寸) */
    HBITMAP  bmp, oldBmp;

    HDC      realDC[WCTL_DC_STACK];   /* 待回贴的真实 DC 栈 */
    int      realTop;

    LONG     paints;             /* 拦到的绘制次数 */
    LONG     blits;              /* 回贴次数 */
} ScaleWin;

static ScaleWin          s_sw[WCTL_SCALE_MAX];
static CRITICAL_SECTION  s_cs;
static BOOL              s_inited = FALSE;
static volatile LONG     s_paints = 0;      /* 全局拦到的绘制次数 */
static volatile LONG     s_startedAt = 0;   /* 最近一次开始缩放的时刻 */
static volatile LONG     s_started = 0;     /* 是否正在尝试缩放 */
static HWND              s_gaveUp = NULL;   /* 试过但拦不到绘制的子窗口(不再折腾) */
static DWORD             s_lastScan = 0;

/* 停止缩放后暂时不销毁的 GDI 对象: 目标程序可能正拿着那个 DC 在画 */
static HDC     s_retireDC[WCTL_RETIRE_MAX];
static HBITMAP s_retireBmp[WCTL_RETIRE_MAX];
static int     s_retireN = 0;

/* ------------------------------------------------------------------ */
/* 真实函数 / IAT 挂钩                                                 */
/* ------------------------------------------------------------------ */
static HDC  (WINAPI *RealBeginPaint)(HWND, LPPAINTSTRUCT) = NULL;
static BOOL (WINAPI *RealEndPaint)(HWND, const PAINTSTRUCT*) = NULL;
static HDC  (WINAPI *RealGetDC)(HWND) = NULL;
static HDC  (WINAPI *RealGetDCEx)(HWND, HRGN, DWORD) = NULL;
static HDC  (WINAPI *RealGetWindowDC)(HWND) = NULL;
static int  (WINAPI *RealReleaseDC)(HWND, HDC) = NULL;

/* 焦点模拟: 目标程序通过这几个 API 判断"我是不是前台/激活窗口" */
static HWND (WINAPI *RealGetForegroundWindow)(void) = NULL;
static HWND (WINAPI *RealGetActiveWindow)(void) = NULL;
static HWND (WINAPI *RealGetFocus)(void) = NULL;
static BOOL (WINAPI *RealGetGUIThreadInfo)(DWORD, PGUITHREADINFO) = NULL;
/* 焦点请求: 焦点模拟开启时拦掉目标的"抢前台"动作 */
static BOOL (WINAPI *RealSetForegroundWindow)(HWND) = NULL;
static BOOL (WINAPI *RealBringWindowToTop)(HWND) = NULL;
static HWND (WINAPI *RealSetActiveWindow)(HWND) = NULL;
static LONG  s_focusBlock = 0;

/* 窗口位置模拟: 目标程序通过这两个 API 问"我的窗口在哪/多大" */
static BOOL (WINAPI *RealGetWindowRect)(HWND, LPRECT) = NULL;
static BOOL (WINAPI *RealGetClientRect)(HWND, LPRECT) = NULL;

typedef struct IatHookDef {
    const char* dll;      /* 提供该函数的系统 DLL(导入表里出现的名字) */
    const char* fn;       /* 函数名 */
    void*       orig;     /* 系统实现 */
    void*       repl;     /* 我们的实现 */
} IatHookDef;

static IatHookDef s_hooks[WCTL_IAT_MAX];
static int        s_hookN = 0;

static void** s_slots[WCTL_SLOT_MAX];   /* 被我们改过的导入表槽位 */
static int    s_slotN = 0;

static LRESULT CALLBACK ScaleChildProc(HWND, UINT, WPARAM, LPARAM);
static HDC  WINAPI HookBeginPaint(HWND, LPPAINTSTRUCT);
static BOOL WINAPI HookEndPaint(HWND, const PAINTSTRUCT*);
static HDC  WINAPI HookGetDC(HWND);
static HDC  WINAPI HookGetDCEx(HWND, HRGN, DWORD);
static HDC  WINAPI HookGetWindowDC(HWND);
static int  WINAPI HookReleaseDC(HWND, HDC);
static HWND WINAPI HookGetForegroundWindow(void);
static HWND WINAPI HookGetActiveWindow(void);
static HWND WINAPI HookGetFocus(void);
static BOOL WINAPI HookGetGUIThreadInfo(DWORD, PGUITHREADINFO);
static BOOL WINAPI HookGetWindowRect(HWND, LPRECT);
static BOOL WINAPI HookGetClientRect(HWND, LPRECT);
static BOOL WINAPI HookSetForegroundWindow(HWND);
static BOOL WINAPI HookBringWindowToTop(HWND);
static HWND WINAPI HookSetActiveWindow(HWND);

/* 焦点模拟开关 + 主窗口是否有效 */
static BOOL FocusSimOn(void)
{
    return g_shm && (g_shm->flags & WCTL_F_FOCUS) != 0 &&
           g_primary && IsWindow(g_primary);
}

static HWND WINAPI HookGetForegroundWindow(void)
{
    if (FocusSimOn()) return g_primary;
    return RealGetForegroundWindow ? RealGetForegroundWindow() : NULL;
}

static HWND WINAPI HookGetActiveWindow(void)
{
    if (FocusSimOn()) return g_primary;
    return RealGetActiveWindow ? RealGetActiveWindow() : NULL;
}

static HWND WINAPI HookGetFocus(void)
{
    HWND h;
    if (FocusSimOn()) {
        h = RealGetFocus ? RealGetFocus() : NULL;
        if (!h) return g_primary;      /* 目标线程没有真实焦点时, 谎报主窗口有焦点 */
        return h;
    }
    return RealGetFocus ? RealGetFocus() : NULL;
}

/* 是不是"本进程的顶层窗口": 内部控件之间的 SetFocus/SetActiveWindow 放行,
   只拦目标把整个窗口往前台带的动作。 */
static BOOL OwnTopLevelWindow(HWND hwnd)
{
    DWORD pid = 0;
    if (!hwnd || !IsWindow(hwnd)) return FALSE;
    if (GetParent(hwnd)) return FALSE;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == GetCurrentProcessId();
}

static void LogFocusBlock(const wchar_t* what, HWND hwnd)
{
    LONG n = InterlockedIncrement(&s_focusBlock);
    if (n <= 5 || (n % 20) == 0)
        WctlLog(L"焦点模拟: 吞掉 %s hwnd=%08X #%ld", what, (unsigned)(ULONG_PTR)hwnd, n);
}

static BOOL WINAPI HookSetForegroundWindow(HWND hwnd)
{
    if (FocusSimOn() && OwnTopLevelWindow(hwnd)) {
        LogFocusBlock(L"SetForegroundWindow", hwnd);
        return TRUE;                     /* 假装成功: 目标以为已经抢到前台 */
    }
    return RealSetForegroundWindow ? RealSetForegroundWindow(hwnd) : FALSE;
}

static BOOL WINAPI HookBringWindowToTop(HWND hwnd)
{
    if (FocusSimOn() && OwnTopLevelWindow(hwnd)) {
        LogFocusBlock(L"BringWindowToTop", hwnd);
        return TRUE;
    }
    return RealBringWindowToTop ? RealBringWindowToTop(hwnd) : FALSE;
}

static HWND WINAPI HookSetActiveWindow(HWND hwnd)
{
    if (FocusSimOn() && OwnTopLevelWindow(hwnd)) {
        LogFocusBlock(L"SetActiveWindow", hwnd);
        return g_primary;
    }
    return RealSetActiveWindow ? RealSetActiveWindow(hwnd) : NULL;
}

static BOOL WINAPI HookGetGUIThreadInfo(DWORD tid, PGUITHREADINFO gi)
{
    BOOL ok;
    if (!gi || !RealGetGUIThreadInfo) return FALSE;
    ok = RealGetGUIThreadInfo(tid, gi);
    if (FocusSimOn()) {
        /* 很多程序用 GetGUIThreadInfo 的 hwndActive/hwndFocus 判断是否在前台 */
        gi->hwndActive = g_primary;
        gi->hwndFocus  = g_primary;
        return TRUE;
    }
    return ok;
}

/* 窗口位置模拟: 重映射期间, 目标程序问"我的窗口在哪/多大"时,
   回答"用户实际看到的那个窗口"(同步=桌面上的查看器, 全屏=占满显示器),
   而不是真实的"屏幕外那个源窗口"。 */
static BOOL PosSimApplies(HWND hwnd)
{
    return WctlRemapPosSimOn() && hwnd == WctlRemapSource();
}

static BOOL WINAPI HookGetWindowRect(HWND hwnd, LPRECT rc)
{
    if (rc && PosSimApplies(hwnd) && WctlRemapSimRect(rc, NULL)) return TRUE;
    return RealGetWindowRect ? RealGetWindowRect(hwnd, rc) : FALSE;
}

static BOOL WINAPI HookGetClientRect(HWND hwnd, LPRECT rc)
{
    if (rc && PosSimApplies(hwnd) && WctlRemapSimRect(NULL, rc)) return TRUE;
    return RealGetClientRect ? RealGetClientRect(hwnd, rc) : FALSE;
}

static BOOL SameDllName(const char* a, const char* b)
{
    const char* pa = a;
    const char* pb = b;
    if (!a || !b) return FALSE;
    for (const char* q = a; *q; q++) if (*q == '\\' || *q == '/') pa = q + 1;
    for (const char* q = b; *q; q++) if (*q == '\\' || *q == '/') pb = q + 1;
    return lstrcmpiA(pa, pb) == 0;
}

static void RecordSlot(void** slot)
{
    if (s_slotN < WCTL_SLOT_MAX) s_slots[s_slotN++] = slot;
}

/* 改一个导入表槽位(需要临时放开写保护) */
static BOOL WriteSlot(void** slot, void* value)
{
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return FALSE;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return TRUE;
}

/* 遍历一个模块的导入表, 把命中挂钩表的槽位换掉 */
static int PatchModuleIat(HMODULE mod, HMODULE self)
{
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos;
    IMAGE_NT_HEADERS* nt;
    IMAGE_IMPORT_DESCRIPTOR* desc;
    DWORD rva;
    int patched = 0;

    if (!mod || mod == self) return 0;
    dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva || rva >= nt->OptionalHeader.SizeOfImage) return 0;

    desc = (IMAGE_IMPORT_DESCRIPTOR*)(base + rva);
    for (; desc->Name; desc++) {
        const char* dllName = (const char*)(base + desc->Name);
        IMAGE_THUNK_DATA* oft = desc->OriginalFirstThunk
                              ? (IMAGE_THUNK_DATA*)(base + desc->OriginalFirstThunk) : NULL;
        IMAGE_THUNK_DATA* ft  = (IMAGE_THUNK_DATA*)(base + desc->FirstThunk);
        int i;
        if (!ft) continue;
        for (i = 0; i < 8192; i++) {
            const char* fnName = NULL;
            void* cur;
            int h;
            if (!ft[i].u1.Function) break;
            if (oft && !IMAGE_SNAP_BY_ORDINAL(oft[i].u1.Ordinal)) {
                IMAGE_IMPORT_BY_NAME* ib = (IMAGE_IMPORT_BY_NAME*)(base + oft[i].u1.AddressOfData);
                fnName = (const char*)ib->Name;
            }
            cur = (void*)(ULONG_PTR)ft[i].u1.Function;
            for (h = 0; h < s_hookN; h++) {
                IatHookDef* hk = &s_hooks[h];
                if (!SameDllName(dllName, hk->dll)) continue;
                if (fnName) { if (lstrcmpiA(fnName, hk->fn) != 0) continue; }
                if (cur != hk->orig) continue;      /* 已被别人挂钩/绑定到别处: 不碰 */
                if (WriteSlot((void**)&ft[i].u1.Function, hk->repl)) {
                    RecordSlot((void**)&ft[i].u1.Function);
                    patched++;
                }
            }
        }
    }
    return patched;
}

/* 扫描当前所有已加载模块(后加载的模块由主循环定期补扫) */
static int PatchAllModules(void)
{
    HANDLE snap;
    MODULEENTRY32W me;
    int added = 0;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                    GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return 0;
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            added += PatchModuleIat((HMODULE)me.modBaseAddr, g_hinst);
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return added;
}

static void AddHook(const char* dll, const char* fn, void* orig, void* repl)
{
    if (s_hookN >= WCTL_IAT_MAX || !orig) return;
    s_hooks[s_hookN].dll  = dll;
    s_hooks[s_hookN].fn   = fn;
    s_hooks[s_hookN].orig = orig;
    s_hooks[s_hookN].repl = repl;
    s_hookN++;
}

static void RestoreSlots(void)
{
    int i;
    for (i = 0; i < s_slotN; i++) {
        void** slot = s_slots[i];
        int h;
        if (!slot) continue;
        for (h = 0; h < s_hookN; h++) {
            if (*slot == s_hooks[h].repl) {   /* 确实是我们挂的才还原, 免踩掉别人的挂钩 */
                WriteSlot(slot, s_hooks[h].orig);
                break;
            }
        }
    }
    s_slotN = 0;
}

BOOL WctlScaleInit(void)
{
    HMODULE u;
    if (s_inited) return TRUE;
    InitializeCriticalSection(&s_cs);
    s_inited = TRUE;

    u = GetModuleHandleW(L"user32.dll");
    if (!u) return FALSE;
    RealBeginPaint  = (HDC  (WINAPI*)(HWND, LPPAINTSTRUCT))GetProcAddress(u, "BeginPaint");
    RealEndPaint    = (BOOL (WINAPI*)(HWND, const PAINTSTRUCT*))GetProcAddress(u, "EndPaint");
    RealGetDC       = (HDC  (WINAPI*)(HWND))GetProcAddress(u, "GetDC");
    RealGetDCEx     = (HDC  (WINAPI*)(HWND, HRGN, DWORD))GetProcAddress(u, "GetDCEx");
    RealGetWindowDC = (HDC  (WINAPI*)(HWND))GetProcAddress(u, "GetWindowDC");
    RealReleaseDC   = (int  (WINAPI*)(HWND, HDC))GetProcAddress(u, "ReleaseDC");
    RealGetForegroundWindow = (HWND (WINAPI*)(void))GetProcAddress(u, "GetForegroundWindow");
    RealGetActiveWindow     = (HWND (WINAPI*)(void))GetProcAddress(u, "GetActiveWindow");
    RealGetFocus            = (HWND (WINAPI*)(void))GetProcAddress(u, "GetFocus");
    RealGetGUIThreadInfo    = (BOOL (WINAPI*)(DWORD, PGUITHREADINFO))GetProcAddress(u, "GetGUIThreadInfo");
    RealGetWindowRect       = (BOOL (WINAPI*)(HWND, LPRECT))GetProcAddress(u, "GetWindowRect");
    RealGetClientRect       = (BOOL (WINAPI*)(HWND, LPRECT))GetProcAddress(u, "GetClientRect");
    RealSetForegroundWindow = (BOOL (WINAPI*)(HWND))GetProcAddress(u, "SetForegroundWindow");
    RealBringWindowToTop    = (BOOL (WINAPI*)(HWND))GetProcAddress(u, "BringWindowToTop");
    RealSetActiveWindow     = (HWND (WINAPI*)(HWND))GetProcAddress(u, "SetActiveWindow");
    if (!RealBeginPaint || !RealEndPaint || !RealGetDC || !RealReleaseDC) return FALSE;

    AddHook("USER32.dll", "BeginPaint",  (void*)RealBeginPaint,  (void*)HookBeginPaint);
    AddHook("USER32.dll", "EndPaint",    (void*)RealEndPaint,    (void*)HookEndPaint);
    AddHook("USER32.dll", "GetDC",       (void*)RealGetDC,       (void*)HookGetDC);
    AddHook("USER32.dll", "GetDCEx",     (void*)RealGetDCEx,     (void*)HookGetDCEx);
    AddHook("USER32.dll", "GetWindowDC", (void*)RealGetWindowDC, (void*)HookGetWindowDC);
    AddHook("USER32.dll", "ReleaseDC",   (void*)RealReleaseDC,   (void*)HookReleaseDC);
    AddHook("USER32.dll", "GetForegroundWindow", (void*)RealGetForegroundWindow, (void*)HookGetForegroundWindow);
    AddHook("USER32.dll", "GetActiveWindow",     (void*)RealGetActiveWindow,     (void*)HookGetActiveWindow);
    AddHook("USER32.dll", "GetFocus",            (void*)RealGetFocus,            (void*)HookGetFocus);
    AddHook("USER32.dll", "GetGUIThreadInfo",    (void*)RealGetGUIThreadInfo,    (void*)HookGetGUIThreadInfo);
    AddHook("USER32.dll", "GetWindowRect",       (void*)RealGetWindowRect,       (void*)HookGetWindowRect);
    AddHook("USER32.dll", "GetClientRect",       (void*)RealGetClientRect,       (void*)HookGetClientRect);
    AddHook("USER32.dll", "SetForegroundWindow", (void*)RealSetForegroundWindow, (void*)HookSetForegroundWindow);
    AddHook("USER32.dll", "BringWindowToTop",    (void*)RealBringWindowToTop,    (void*)HookBringWindowToTop);
    AddHook("USER32.dll", "SetActiveWindow",     (void*)RealSetActiveWindow,     (void*)HookSetActiveWindow);
    PatchAllModules();
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* 登记表                                                              */
/* ------------------------------------------------------------------ */
static ScaleWin* FindByHwndLocked(HWND hwnd)
{
    int i;
    for (i = 0; i < WCTL_SCALE_MAX; i++)
        if (s_sw[i].used && s_sw[i].child == hwnd) return &s_sw[i];
    return NULL;
}

static ScaleWin* FindByDCLocked(HDC dc)
{
    int i;
    if (!dc) return NULL;
    for (i = 0; i < WCTL_SCALE_MAX; i++)
        if (s_sw[i].used && s_sw[i].memDC == dc) return &s_sw[i];
    return NULL;
}

static void UnregisterLocked(ScaleWin* sw)
{
    if (IsWindow(sw->child)) {
        LONG_PTR cur = GetWindowLongPtrW(sw->child, GWLP_WNDPROC);
        if (cur == (LONG_PTR)ScaleChildProc && sw->orig)
            SetWindowLongPtrW(sw->child, GWLP_WNDPROC, (LONG_PTR)sw->orig);
    }
    /* GDI 对象不立刻销毁: 目标程序可能正拿着这个 DC 在画 */
    if (sw->memDC) {
        if (sw->oldBmp) SelectObject(sw->memDC, sw->oldBmp);
        if (s_retireN < WCTL_RETIRE_MAX) {
            s_retireDC[s_retireN] = sw->memDC;
            s_retireBmp[s_retireN] = sw->bmp;
            s_retireN++;
        } else {
            if (sw->bmp) DeleteObject(sw->bmp);
            DeleteDC(sw->memDC);
        }
    } else if (sw->bmp) {
        DeleteObject(sw->bmp);
    }
    if (sw->ctx) { sw->ctx->scaleOn = FALSE; sw->ctx->scaleWasOn = TRUE; }
    memset(sw, 0, sizeof(*sw));
}

/* ------------------------------------------------------------------ */
/* 缩放几何: 内容 -> 可见区域                                          */
/* ------------------------------------------------------------------ */
/* 子窗口客户区里"真正能被看到"的那块(受父窗口客户区限制) */
static void VisibleSizeLocked(ScaleWin* sw, int* visW, int* visH)
{
    HWND parent = GetParent(sw->child);
    RECT pr;
    int aw, ah;

    *visW = (int)sw->cw;
    *visH = (int)sw->ch;
    if (!parent || !GetClientRect(parent, &pr)) return;
    /* 子窗口窗口左上角在父客户区的 (pinWinX, pinWinY), 客户区再往里 cliOff */
    aw = pr.right  - (int)sw->pinWinX - (int)sw->cliOffX;
    ah = pr.bottom - (int)sw->pinWinY - (int)sw->cliOffY;
    if (aw < 1) aw = 1;
    if (ah < 1) ah = 1;
    if (aw < *visW) *visW = aw;
    if (ah < *visH) *visH = ah;
}

/* 等比缩放的目标矩形(居中, 多出来的地方补黑边) */
static BOOL ComputeDstLocked(ScaleWin* sw, RECT* dst, int* visW, int* visH)
{
    int vw = 0, vh = 0, dw, dh, dx, dy;

    if (sw->cw <= 0 || sw->ch <= 0) return FALSE;
    VisibleSizeLocked(sw, &vw, &vh);
    if (vw < 1 || vh < 1) return FALSE;
    /* 交叉相乘选较小的那个比例, 保证内容完整可见 */
    if ((LONGLONG)vw * sw->ch <= (LONGLONG)vh * sw->cw) {
        dw = vw;
        dh = (int)((LONGLONG)sw->ch * vw / sw->cw);
    } else {
        dh = vh;
        dw = (int)((LONGLONG)sw->cw * vh / sw->ch);
    }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    dx = (vw - dw) / 2;
    dy = (vh - dh) / 2;
    dst->left = dx; dst->top = dy; dst->right = dx + dw; dst->bottom = dy + dh;
    if (visW) *visW = vw;
    if (visH) *visH = vh;
    return TRUE;
}

/* 把离屏内容缩放贴到真实 DC 上(调用时必须已持有 s_cs) */
static void BlitScaledLocked(ScaleWin* sw, HDC realDC)
{
    RECT dst;
    int vw = 0, vh = 0, old;
    LONG n;

    if (!realDC || !sw->memDC) return;
    if (!ComputeDstLocked(sw, &dst, &vw, &vh)) return;

    /* 先把可见区刷黑(等比缩放时上下/左右就是黑边), 免得残留上一帧 */
    {
        RECT fill;
        fill.left = 0; fill.top = 0; fill.right = vw; fill.bottom = vh;
        FillRect(realDC, &fill, (HBRUSH)GetStockObject(BLACK_BRUSH));
    }
    old = SetStretchBltMode(realDC, HALFTONE);
    SetBrushOrgEx(realDC, 0, 0, NULL);
    StretchBlt(realDC, dst.left, dst.top, dst.right - dst.left, dst.bottom - dst.top,
               sw->memDC, 0, 0, (int)sw->cw, (int)sw->ch, SRCCOPY);
    SetStretchBltMode(realDC, old);

    n = ++sw->blits;
    if (n == 1 || (n % 100) == 0)
        WctlLogAlways(L"wctl: 内容缩放 %ldx%ld -> %dx%d 显示 (第 %ld 帧)",
                      (long)sw->cw, (long)sw->ch,
                      dst.right - dst.left, dst.bottom - dst.top, n);
}

/* 把"缩放显示"上的坐标换回内容坐标(鼠标消息要按内容坐标交给目标程序) */
static BOOL MapPointLocked(ScaleWin* sw, int* px, int* py)
{
    RECT d;
    int x, y, w, h;

    if (!ComputeDstLocked(sw, &d, NULL, NULL)) return FALSE;
    w = d.right - d.left;
    h = d.bottom - d.top;
    if (w < 1 || h < 1) return FALSE;
    x = *px; y = *py;
    if (x < d.left) x = d.left;
    if (x > d.right) x = d.right;
    if (y < d.top) y = d.top;
    if (y > d.bottom) y = d.bottom;
    *px = (int)((LONGLONG)(x - d.left) * sw->cw / w);
    *py = (int)((LONGLONG)(y - d.top) * sw->ch / h);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* 挂钩实现: 只对登记过的那一个子窗口生效                               */
/* ------------------------------------------------------------------ */
static HDC RedirectLocked(HWND hwnd, HDC real)
{
    ScaleWin* sw;
    HDC mem = NULL;

    if (!real) return real;
    EnterCriticalSection(&s_cs);
    sw = FindByHwndLocked(hwnd);
    if (sw && sw->memDC) {
        if (sw->realTop < WCTL_DC_STACK) sw->realDC[sw->realTop++] = real;
        sw->paints++;
        InterlockedIncrement((LONG volatile*)&s_paints);
        mem = sw->memDC;
    }
    LeaveCriticalSection(&s_cs);
    if (!mem) {
        /* 显示重映射期间: 目标程序的绘制也改画进重映射的抓取位图
           (源窗口在屏幕外, 系统拿不到它的实时画面) */
        HDC rm = NULL;
        if (WctlRemapPaintBegin(hwnd, &rm, NULL, NULL)) return rm;
    }
    return mem ? mem : real;
}

static HDC WINAPI HookBeginPaint(HWND hwnd, LPPAINTSTRUCT ps)
{
    HDC real, mem = NULL;
    LONG cw = 0, ch = 0;
    ScaleWin* sw;

    real = RealBeginPaint(hwnd, ps);
    if (!real || !ps) return real;

    EnterCriticalSection(&s_cs);
    sw = FindByHwndLocked(hwnd);
    if (sw && sw->memDC) {
        if (sw->realTop < WCTL_DC_STACK) sw->realDC[sw->realTop++] = real;
        sw->paints++;
        InterlockedIncrement((LONG volatile*)&s_paints);
        mem = sw->memDC;
        cw = sw->cw;
        ch = sw->ch;
    }
    LeaveCriticalSection(&s_cs);

    if (!mem) {
        /* 重映射: 换成抓取位图, 并告诉目标"整个客户区都要重画" */
        HDC rm = NULL;
        LONG rw = 0, rh = 0;
        if (WctlRemapPaintBegin(hwnd, &rm, &rw, &rh)) {
            ps->hdc = rm;
            ps->rcPaint.left = 0;
            ps->rcPaint.top = 0;
            ps->rcPaint.right = rw;
            ps->rcPaint.bottom = rh;
            return rm;
        }
        return real;
    }
    /* 交给目标程序的是"内容尺寸"的离屏 DC: 它照旧画完整的一帧 */
    ps->hdc = mem;
    ps->rcPaint.left = 0;
    ps->rcPaint.top = 0;
    ps->rcPaint.right = cw;
    ps->rcPaint.bottom = ch;
    return mem;
}

static BOOL WINAPI HookEndPaint(HWND hwnd, const PAINTSTRUCT* ps)
{
    HDC real = NULL;
    PAINTSTRUCT copy;
    ScaleWin* sw;

    EnterCriticalSection(&s_cs);
    sw = FindByHwndLocked(hwnd);
    if (sw && sw->realTop > 0) {
        real = sw->realDC[--sw->realTop];
        BlitScaledLocked(sw, real);
    }
    LeaveCriticalSection(&s_cs);

    if (!real) {
        WctlRemapPaintDone(hwnd);      /* 重映射: 这一帧画完了, 交给查看器 */
        return RealEndPaint(hwnd, ps);
    }
    /* 交回真实 DC, 让系统正常结束这次绘制 */
    if (ps) { copy = *ps; copy.hdc = real; ps = &copy; }
    return RealEndPaint(hwnd, ps);
}

static HDC WINAPI HookGetDC(HWND hwnd)       { return RedirectLocked(hwnd, RealGetDC(hwnd)); }
static HDC WINAPI HookGetWindowDC(HWND hwnd) { return RedirectLocked(hwnd, RealGetWindowDC(hwnd)); }

static HDC WINAPI HookGetDCEx(HWND hwnd, HRGN rgn, DWORD flags)
{
    return RedirectLocked(hwnd, RealGetDCEx(hwnd, rgn, flags));
}

static int WINAPI HookReleaseDC(HWND hwnd, HDC hdc)
{
    HDC real = NULL;
    ScaleWin* sw;

    EnterCriticalSection(&s_cs);
    sw = FindByHwndLocked(hwnd);
    if (!sw) sw = FindByDCLocked(hdc);
    if (sw && hdc == sw->memDC && sw->realTop > 0) {
        real = sw->realDC[--sw->realTop];
        BlitScaledLocked(sw, real);
    }
    LeaveCriticalSection(&s_cs);

    if (real) { RealReleaseDC(hwnd, real); return 1; }
    WctlRemapPaintDone(hwnd);          /* 重映射: GetDC 取走的 DC 用完了 */
    return RealReleaseDC(hwnd, hdc);
}

/* ------------------------------------------------------------------ */
/* 内容视图子窗口的窗口过程: 钉住尺寸 / 换算鼠标坐标                    */
/* ------------------------------------------------------------------ */
static BOOL IsPositionMsg(UINT m)
{
    return m == WM_MOUSEMOVE || m == WM_LBUTTONDOWN || m == WM_LBUTTONUP ||
           m == WM_LBUTTONDBLCLK || m == WM_RBUTTONDOWN || m == WM_RBUTTONUP ||
           m == WM_RBUTTONDBLCLK || m == WM_MBUTTONDOWN || m == WM_MBUTTONUP ||
           m == WM_MBUTTONDBLCLK || m == WM_XBUTTONDOWN || m == WM_XBUTTONUP ||
           m == WM_XBUTTONDBLCLK;
}

static LRESULT CALLBACK ScaleChildProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    WNDPROC orig = NULL;
    LONG pinX = 0, pinY = 0, pinW = 0, pinH = 0;
    BOOL  known = FALSE;
    int   mx = 0, my = 0;
    BOOL  mapped = FALSE;

    EnterCriticalSection(&s_cs);
    {
        ScaleWin* sw = FindByHwndLocked(hwnd);
        if (sw) {
            orig  = sw->orig;
            pinX  = sw->pinWinX; pinY = sw->pinWinY;
            pinW  = sw->pinW;    pinH = sw->pinH;
            known = TRUE;
            if (IsPositionMsg(msg)) {
                mx = (short)LOWORD(lp);
                my = (short)HIWORD(lp);
                mapped = MapPointLocked(sw, &mx, &my);
            }
        }
    }
    LeaveCriticalSection(&s_cs);

    if (!known || !orig) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_WINDOWPOSCHANGING: {
        /* 尺寸/位置钉死: 目标程序在父窗口变化时想重排这个子窗口, 一律按回去 */
        WINDOWPOS* p = (WINDOWPOS*)lp;
        if (p) {
            p->x  = (int)pinX; p->y  = (int)pinY;
            p->cx = (int)pinW; p->cy = (int)pinH;
            p->flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
        }
        break;
    }
    case WM_SIZE:
        /* 尺寸根本没变(被我们钉住了), 不要把"新客户区"告诉目标程序:
           它一旦按新尺寸重排/重算画面, 我们拿到的就不是完整内容了 */
        return 0;

    default:
        if (mapped) {
            int x = mx, y = my;
            if (x < 0) x = 0;
            if (x > 32767) x = 32767;
            if (y < 0) y = 0;
            if (y > 32767) y = 32767;
            lp = (LPARAM)MAKELPARAM(x, y);
        }
        break;
    }
    return CallWindowProcW(orig, hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* 开始 / 停止                                                         */
/* ------------------------------------------------------------------ */
static BOOL BeginScale(WndCtx* c, HWND child)
{
    ScaleWin* sw = NULL;
    RECT cr, wr;
    POINT p, q;
    HWND parent;
    HDC screen = NULL;
    HDC memDC = NULL;
    HBITMAP bmp = NULL, oldBmp = NULL;
    WNDPROC orig;
    LONG pinWinX, pinWinY, pinW, pinH, cliOffX, cliOffY;
    int cw, ch, i;

    if (!s_inited || !c || !IsWindow(child)) return FALSE;
    if (!GetClientRect(child, &cr)) return FALSE;
    cw = cr.right; ch = cr.bottom;
    if (cw < 64 || ch < 64) return FALSE;            /* 太小的窗口不掺和 */
    parent = GetParent(child);
    if (!parent) return FALSE;

    p.x = 0; p.y = 0;
    if (!ClientToScreen(child, &p)) return FALSE;    /* 子客户区原点(屏幕坐标) */
    q.x = 0; q.y = 0;
    if (!ClientToScreen(parent, &q)) return FALSE;   /* 父客户区原点(屏幕坐标) */
    if (!GetWindowRect(child, &wr)) return FALSE;

    EnterCriticalSection(&s_cs);
    if (FindByHwndLocked(child)) {                   /* 已经登记过 */
        LeaveCriticalSection(&s_cs);
        c->scaleOn = TRUE;
        return TRUE;
    }
    LeaveCriticalSection(&s_cs);

    /* GDI 对象在锁外创建, 免得把目标程序的绘制线程堵住 */
    screen = GetDC(NULL);
    if (!screen) return FALSE;
    memDC = CreateCompatibleDC(screen);
    bmp   = CreateCompatibleBitmap(screen, cw, ch);
    ReleaseDC(NULL, screen);
    if (!memDC || !bmp) {
        if (bmp) DeleteObject(bmp);
        if (memDC) DeleteDC(memDC);
        return FALSE;
    }
    oldBmp = (HBITMAP)SelectObject(memDC, bmp);
    {   /* 先刷黑: 第一帧缩放上来之前不要花屏 */
        RECT r;
        r.left = 0; r.top = 0; r.right = cw; r.bottom = ch;
        FillRect(memDC, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
    }
    orig = (WNDPROC)GetWindowLongPtrW(child, GWLP_WNDPROC);

    pinWinX = wr.left - q.x;
    pinWinY = wr.top  - q.y;
    pinW    = wr.right - wr.left;
    pinH    = wr.bottom - wr.top;
    cliOffX = p.x - wr.left;
    cliOffY = p.y - wr.top;

    EnterCriticalSection(&s_cs);
    for (i = 0; i < WCTL_SCALE_MAX; i++) {
        if (!s_sw[i].used) {
            sw = &s_sw[i];
            memset(sw, 0, sizeof(*sw));
            sw->used    = TRUE;
            sw->child   = child;
            sw->ctx     = c;
            sw->orig    = orig;
            sw->cw      = cw;
            sw->ch      = ch;
            sw->pinWinX = pinWinX;
            sw->pinWinY = pinWinY;
            sw->pinW    = pinW;
            sw->pinH    = pinH;
            sw->cliOffX = cliOffX;
            sw->cliOffY = cliOffY;
            sw->memDC   = memDC;
            sw->bmp     = bmp;
            sw->oldBmp  = oldBmp;
            break;
        }
    }
    LeaveCriticalSection(&s_cs);
    if (!sw) {                                       /* 没坑了: 回滚 */
        if (oldBmp) SelectObject(memDC, oldBmp);
        DeleteObject(bmp);
        DeleteDC(memDC);
        return FALSE;
    }

    /* 子类化 + 把子窗口按原尺寸钉住(在锁外做: 这两步都会给目标窗口发消息) */
    if (orig) SetWindowLongPtrW(child, GWLP_WNDPROC, (LONG_PTR)ScaleChildProc);
    SetWindowPos(child, NULL, (int)pinWinX, (int)pinWinY, (int)pinW, (int)pinH,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);

    s_paints = 0;
    s_startedAt = (LONG)GetTickCount();
    s_started = 1;
    c->scaleOn = TRUE;
    WctlLogAlways(L"wctl: 已启用内容缩放 (内容视图子窗口 %08X, 内容 %dx%d)",
                  (unsigned)(ULONG_PTR)child, cw, ch);
    return TRUE;
}

void WctlScaleEnd(WndCtx* c)
{
    int i;
    if (!c) return;
    c->scaleOn = FALSE;
    if (!s_inited) return;
    EnterCriticalSection(&s_cs);
    for (i = 0; i < WCTL_SCALE_MAX; i++) {
        if (!s_sw[i].used || s_sw[i].ctx != c) continue;
        UnregisterLocked(&s_sw[i]);
    }
    LeaveCriticalSection(&s_cs);
}

/* 窗口(或它的内容视图子窗口)正在被销毁: 立刻收工 —— 必须在 WndCtx 被释放之前调用,
   否则 ScaleWin 里记的 ctx 会变成野指针。 */
void WctlScaleEndByHwnd(HWND hwnd)
{
    int i;
    if (!s_inited || !hwnd) return;
    EnterCriticalSection(&s_cs);
    for (i = 0; i < WCTL_SCALE_MAX; i++) {
        ScaleWin* sw = &s_sw[i];
        if (!sw->used) continue;
        if (sw->child == hwnd || (sw->ctx && sw->ctx->hwnd == hwnd))
            UnregisterLocked(sw);
    }
    LeaveCriticalSection(&s_cs);
}

void WctlScaleStopAll(void)
{
    int i;
    if (!s_inited) return;
    EnterCriticalSection(&s_cs);
    for (i = 0; i < WCTL_SCALE_MAX; i++) {
        if (!s_sw[i].used) continue;
        UnregisterLocked(&s_sw[i]);
    }
    LeaveCriticalSection(&s_cs);
}

void WctlScaleSync(WndCtx* c)
{
    HWND child = NULL;

    if (!s_inited || !c || !IsWindow(c->hwnd)) return;
    if (!(WctlFlags() & WCTL_F_SCALECONTENT)) {
        WctlScaleEnd(c);
        s_gaveUp = NULL;      /* 开关关掉再打开 = 用户想再试一次 */
        return;
    }
    if (!c->contentFixed) {
        /* 接管那一刻还没出现"内容视图子窗口"(比如教室软件要等老师开始广播才出画面),
           这里平时也盯一眼: 它一旦变成霸屏窗口 + 出现内容视图子窗口就补上判定。 */
        HWND ch = NULL;
        if (!WctlContentLooksFixed(c->hwnd, &ch)) return;
        c->contentFixed = TRUE;
        c->contentChild = ch;
        if (ch) {
            RECT cr;
            if (GetClientRect(ch, &cr) && cr.right > 0 && cr.bottom > 0) {
                c->contentW = cr.right;
                c->contentH = cr.bottom;
            }
        }
        WctlLogAlways(L"wctl: 补检测到内容视图子窗口 %08X (内容 %ldx%ld)",
                      (unsigned)(ULONG_PTR)ch, (long)c->contentW, (long)c->contentH);
    }
    if (c->scaleOn && c->contentChild && IsWindow(c->contentChild)) return;   /* 已就绪 */

    child = (c->contentChild && IsWindow(c->contentChild))
          ? c->contentChild : WctlFindContentChild(c->hwnd);
    if (!child) { WctlScaleEnd(c); return; }
    if (child == s_gaveUp) return;      /* 这个子窗口试过且拦不到绘制: 不再折腾 */
    c->contentChild = child;
    {
        RECT ccr;
        if (GetClientRect(child, &ccr)) {
            if (ccr.right > 0)  c->contentW = ccr.right;
            if (ccr.bottom > 0) c->contentH = ccr.bottom;
        }
    }
    BeginScale(c, child);
}

void WctlScaleTick(void)
{
    DWORD now;

    if (!s_inited) return;
    now = GetTickCount();
    /* 后加载的 DLL(插件/迟绑定)也要挂上: 定期补扫模块导入表 */
    if (now - s_lastScan > 2000) {
        s_lastScan = now;
        PatchAllModules();
    }
    /* 健康检查: 已经开始缩放, 但 3 秒都没拦到一次绘制 —— 目标不是用 GDI 画的
       (DirectX 独占 / UpdateLayeredWindow 等), 这条路对它无效, 退回"保持原始内容尺寸"。 */
    if (s_started && s_paints == 0 && s_startedAt &&
        (LONG)(now - (DWORD)s_startedAt) > 3000) {
        int i;
        HWND gave = NULL;
        for (i = 0; i < WCTL_SCALE_MAX; i++) {
            if (s_sw[i].used) { gave = s_sw[i].child; break; }
        }
        s_started = 0;
        s_startedAt = 0;
        if (!gave) return;          /* 这一轮缩放早就收了(窗口没了/开关关了), 不用回退 */
        s_gaveUp = gave;
        WctlLogAlways(L"wctl: 内容视图子窗口不是用 GDI 画的(没拦到任何绘制), "
                      L"内容缩放对它无效 -> 退回保持原始内容尺寸");
        WctlScaleStopAll();
        WctlAutoSize();             /* 这次会走"保持原始内容尺寸"的分支 */
    }
}

BOOL WctlScaleIsOn(WndCtx* c)   { return (c && c->scaleOn) ? TRUE : FALSE; }

void WctlScaleShutdown(void)
{
    if (!s_inited) return;
    WctlScaleStopAll();
    EnterCriticalSection(&s_cs);
    {
        int i;
        for (i = 0; i < s_retireN; i++) {
            if (s_retireBmp[i]) DeleteObject(s_retireBmp[i]);
            if (s_retireDC[i]) DeleteDC(s_retireDC[i]);
        }
        s_retireN = 0;
    }
    LeaveCriticalSection(&s_cs);
    RestoreSlots();
    s_hookN = 0;
    s_inited = FALSE;
}
