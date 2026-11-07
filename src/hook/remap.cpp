/* remap.cpp -- 显示重映射 (第二种内置实现)
 *
 * 第一种实现(scale.cpp): 把窗口缩小, 目标仍然"自己画在自己窗口里", 我们只把它的
 *   绘制重定向到离屏 DC, 再等比缩放贴回窗口(黑边填充)。
 * 第二种实现(本文件): 不要求目标程序配合绘制 ——
 *   1) 把源窗口按**原始内容尺寸**摆到所有显示器之外(坐标为负/在虚拟桌面外),
 *      目标程序照常运行、照常收帧、照常绘制, 只是没人看得见;
 *   2) 在桌面上另开一个普通窗口(查看器), 用 PrintWindow/BitBlt (或 DWM 缩略图)
 *      把源窗口的客户区抓回来, 等比缩放显示在查看器里, 多余部分用黑色填充;
 *   3) 只有画面矩形内的鼠标点击/滚轮/键盘才有效, 坐标反向换算后直接投递给源窗口,
 *      于是"在查看器里的操作"等于"在原窗口里的操作"。
 *
 * 和第一种实现的区别:
 *   - 第一种: 源窗口在桌面上(缩小), 画面被我们重绘回它自己窗口;
 *   - 第二种: 源窗口完全离屏(尺寸不变), 画面搬到另一个窗口, 源窗口不再显示在桌面。
 */
#include "wctl_core.h"
#include <windowsx.h>
#include <wchar.h>
#include <imm.h>

#define RM_CLASS            L"WctlRemapViewerW"
#define RM_TIMER            0x5721
#define RM_CAPTURE_MS       40      /* 抓帧周期(~25fps) */
#define RM_ENFORCE_MS       300     /* 离屏几何纠偏周期 */

/* DWM 缩略图动态加载(不依赖 dwmapi.lib, 32/64 位老系统也能编译) */
#define RM_DWM_TNP_RECTDESTINATION      0x00000001
#define RM_DWM_TNP_RECTSOURCE           0x00000002
#define RM_DWM_TNP_OPACITY              0x00000004
#define RM_DWM_TNP_VISIBLE              0x00000008
#define RM_DWM_TNP_SOURCECLIENTAREAONLY 0x00000010

#define PW_CLIENTONLY_          0x00000001
#define PW_RENDERFULLCONTENT_   0x00000002

typedef struct RmThumbProps {
    DWORD dwFlags;
    RECT  rcDestination;
    RECT  rcSource;
    BYTE  opacity;
    BOOL  fVisible;
    BOOL  fSourceClientAreaOnly;
} RmThumbProps;

typedef HRESULT (WINAPI *PFN_DwmRegisterThumbnail)(HWND, HWND, HANDLE*);
typedef HRESULT (WINAPI *PFN_DwmUpdateThumbnailProperties)(HANDLE, const RmThumbProps*);
typedef HRESULT (WINAPI *PFN_DwmUnregisterThumbnail)(HANDLE);

static HMODULE s_dwmDll = NULL;
static BOOL    s_dwmTried = FALSE;
static PFN_DwmRegisterThumbnail          pDwmRegister = NULL;
static PFN_DwmUpdateThumbnailProperties  pDwmUpdate   = NULL;
static PFN_DwmUnregisterThumbnail        pDwmUnregister = NULL;

typedef struct RemapState {
    BOOL    active;
    BOOL    ending;
    HWND    target;          /* 主窗口(源) */
    HWND    viewer;          /* 桌面查看器 */
    HWND    capture;         /* 真正被抓取的窗口(内容子窗口优先) */
    HWND    input;           /* 接收转发输入的窗口 */

    RECT    savedRect;
    LONG_PTR savedStyle;
    LONG_PTR savedExStyle;
    BOOL    savedZoomed;
    BOOL    savedIconic;
    BOOL    savedVisible;

    LONG    cw, ch;          /* 源内容尺寸(像素) */
    RECT    dst;             /* 画面在查看器客户区里的目标矩形(等比 + 居中) */

    HANDLE  thumb;           /* DWM 缩略图句柄 */
    BOOL    useDwm;

    HDC     capDC;
    HBITMAP capBmp;
    HBITMAP capOld;
    LONG    capW, capH;

    /* 绘制重定向用的位图: 目标程序画进 paintDC, 画完由 PaintDone 拷到 capDC */
    HDC     paintDC;
    HBITMAP paintBmp;
    HBITMAP paintOld;
    DWORD   lastEnforce;
    LONG    failCount;
    LONG    mode;            /* WCTL_REMAPCAP_* */

    LONG    scaleMode;       /* WCTL_REMAPSCALE_*: 启动时的缩放规则(同步/全屏) */
    RECT    monRect;         /* 源窗口原来所在的显示器矩形(全屏模拟/查看器全屏用) */

    /* 查看器自己被"全屏"时的还原信息 */
    BOOL     vFullOn;
    RECT     vSavedRect;
    LONG_PTR vSavedStyle;
    LONG     vTop;           /* 查看器当前是否已置顶(-1=未知) 只在变化时动 Z 序 */

    /* 查看器画面双缓冲(防闪烁): 先在内存里画好黑底+画面, 再一次贴上屏 */
    HDC     viewDC;
    HBITMAP viewBmp;
    HBITMAP viewOld;
    LONG    viewW, viewH;
} RemapState;

static RemapState s_rm;
static BOOL      s_classReg = FALSE;

/* 绘制重定向: 目标程序自己 BeginPaint/GetDC 时把内容画进抓取位图。
   源窗口被搬到屏幕外之后, PrintWindow/DWM 都拿不到它的实时画面(只有旧快照),
   所以在进程内直接截获它的绘制是最可靠的办法。 */
static CRITICAL_SECTION s_frameCs;
static BOOL      s_frameCsInit = FALSE;
static DWORD     s_lastPaint = 0;
static DWORD     s_paintOwner = 0;   /* 正在往抓取位图上画的线程 */
static int       s_paintDepth = 0;
static BOOL      s_everPainted = FALSE; /* 目标是否自己画过一帧(画过就再也不用 PrintWindow 兜底, 否则旧快照会闪) */
static DWORD     s_lastInv = 0;      /* 上次让查看器重画的时刻(限频, 防闪烁) */

/* 输入法(imm32)动态加载: 查看器收到输入法的"最终结果字符串"后, 逐字以 WM_CHAR
   投递给源窗口。目标程序自己用 ImmGetContext 拿不到查看器的输入上下文, 所以只能
   把组字结果翻译成字符消息转发过去。 */
typedef LONG (WINAPI *PFN_ImmGetCompositionStringW)(HIMC, DWORD, LPVOID, DWORD);
static PFN_ImmGetCompositionStringW s_pImmGetComp = NULL;
static BOOL s_immTried = FALSE;

static LONG ImmGetCompString(HIMC himc, DWORD type, LPVOID buf, DWORD len)
{
    if (!s_immTried) {
        s_immTried = TRUE;
        HMODULE h = LoadLibraryW(L"imm32.dll");
        if (h) s_pImmGetComp = (PFN_ImmGetCompositionStringW)(void*)GetProcAddress(h, "ImmGetCompositionStringW");
    }
    return s_pImmGetComp ? s_pImmGetComp(himc, type, buf, len) : 0;
}

static void FrameLock(void)
{
    if (!s_frameCsInit) {
        InitializeCriticalSection(&s_frameCs);
        s_frameCsInit = TRUE;
    }
    EnterCriticalSection(&s_frameCs);
}
static void FrameUnlock(void) { LeaveCriticalSection(&s_frameCs); }

static void ReleaseDib(void);
static void UpdateLayout(void);
static void UpdateThumb(void);
static BOOL CaptureFrame(void);
static void EnforceGeometry(WndCtx* c);

static void LoadDwm(void)
{
    if (s_dwmTried) return;
    s_dwmTried = TRUE;
    s_dwmDll = LoadLibraryW(L"dwmapi.dll");
    if (!s_dwmDll) return;
    pDwmRegister   = (PFN_DwmRegisterThumbnail)(void*)GetProcAddress(s_dwmDll, "DwmRegisterThumbnail");
    pDwmUpdate     = (PFN_DwmUpdateThumbnailProperties)(void*)GetProcAddress(s_dwmDll, "DwmUpdateThumbnailProperties");
    pDwmUnregister = (PFN_DwmUnregisterThumbnail)(void*)GetProcAddress(s_dwmDll, "DwmUnregisterThumbnail");
}

/* ------------------------------------------------------------------ */
/* 几何: 内容 -> 查看器                                                */
/* ------------------------------------------------------------------ */
static void UpdateContentSize(void)
{
    RECT cr;
    if (!s_rm.capture || !IsWindow(s_rm.capture)) return;
    if (!GetClientRect(s_rm.capture, &cr)) return;
    if (cr.right < 1 || cr.bottom < 1) return;
    s_rm.cw = cr.right;
    s_rm.ch = cr.bottom;
}

static void UpdateLayout(void)
{
    RECT cr;
    LONG vw, vh, dw, dh;

    memset(&s_rm.dst, 0, sizeof(s_rm.dst));
    if (!s_rm.viewer || !IsWindow(s_rm.viewer)) return;
    if (!GetClientRect(s_rm.viewer, &cr)) return;
    vw = cr.right;
    vh = cr.bottom;
    if (vw < 1 || vh < 1 || s_rm.cw < 1 || s_rm.ch < 1) return;

    /* 交叉相乘选较小的比例: 画面完整可见, 多出来的地方补黑边 */
    if ((LONGLONG)vw * s_rm.ch <= (LONGLONG)vh * s_rm.cw) {
        dw = vw;
        dh = (LONG)((LONGLONG)s_rm.ch * vw / s_rm.cw);
    } else {
        dh = vh;
        dw = (LONG)((LONGLONG)s_rm.cw * vh / s_rm.ch);
    }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    s_rm.dst.left   = (vw - dw) / 2;
    s_rm.dst.top    = (vh - dh) / 2;
    s_rm.dst.right  = s_rm.dst.left + dw;
    s_rm.dst.bottom = s_rm.dst.top + dh;

    if (s_rm.useDwm) UpdateThumb();
}

static void UpdateThumb(void)
{
    RmThumbProps tp;
    if (!s_rm.thumb || !pDwmUpdate) return;
    memset(&tp, 0, sizeof(tp));
    tp.dwFlags = RM_DWM_TNP_RECTDESTINATION | RM_DWM_TNP_VISIBLE |
                 RM_DWM_TNP_OPACITY | RM_DWM_TNP_SOURCECLIENTAREAONLY;
    tp.rcDestination = s_rm.dst;
    tp.opacity = 255;
    tp.fVisible = TRUE;
    tp.fSourceClientAreaOnly = TRUE;
    pDwmUpdate(s_rm.thumb, &tp);
}

static BOOL TryDwm(void)
{
    HANDLE thumb = NULL;
    HRESULT hr;

    if (s_rm.useDwm) return TRUE;
    if (!s_rm.viewer || !s_rm.target) return FALSE;
    LoadDwm();
    if (!pDwmRegister) return FALSE;

    hr = pDwmRegister(s_rm.viewer, s_rm.target, &thumb);
    if (FAILED(hr) || !thumb) return FALSE;

    s_rm.thumb = thumb;
    s_rm.useDwm = TRUE;
    ReleaseDib();                 /* 不需要内存位图了 */
    UpdateLayout();
    InvalidateRect(s_rm.viewer, NULL, FALSE);
    WctlLogAlways(L"显示重映射: 使用 DWM 缩略图合成 (img hwnd=%08X)",
                  (unsigned)(ULONG_PTR)s_rm.target);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* 抓帧 (PrintWindow / BitBlt)                                         */
/* ------------------------------------------------------------------ */
static BOOL EnsureDib(void)
{
    RECT cr;
    LONG w, h;
    HDC screen;
    BITMAPINFO bmi;
    void* bits = NULL;

    if (!s_rm.capture || !IsWindow(s_rm.capture)) return FALSE;
    if (!GetClientRect(s_rm.capture, &cr)) return FALSE;
    w = cr.right;
    h = cr.bottom;
    if (w < 1 || h < 1) return FALSE;
    if (s_rm.capBmp && s_rm.capW == w && s_rm.capH == h) return TRUE;

    ReleaseDib();
    screen = GetDC(NULL);
    if (!screen) return FALSE;

    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h;      /* top-down */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    s_rm.capDC = CreateCompatibleDC(screen);
    s_rm.capBmp = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (s_rm.capDC && s_rm.capBmp) {
        bits = NULL;
        s_rm.paintDC = CreateCompatibleDC(screen);
        s_rm.paintBmp = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    }
    ReleaseDC(NULL, screen);
    if (!s_rm.capDC || !s_rm.capBmp || !s_rm.paintDC || !s_rm.paintBmp) {
        ReleaseDib();
        return FALSE;
    }
    s_rm.capOld = (HBITMAP)SelectObject(s_rm.capDC, s_rm.capBmp);
    s_rm.paintOld = (HBITMAP)SelectObject(s_rm.paintDC, s_rm.paintBmp);
    s_rm.capW = w;
    s_rm.capH = h;
    return TRUE;
}

static void ReleaseDib(void)
{
    if (s_rm.capDC) {
        if (s_rm.capOld) SelectObject(s_rm.capDC, s_rm.capOld);
        DeleteDC(s_rm.capDC);
    }
    if (s_rm.capBmp) DeleteObject(s_rm.capBmp);
    if (s_rm.paintDC) {
        if (s_rm.paintOld) SelectObject(s_rm.paintDC, s_rm.paintOld);
        DeleteDC(s_rm.paintDC);
    }
    if (s_rm.paintBmp) DeleteObject(s_rm.paintBmp);
    s_rm.capDC = NULL;
    s_rm.capBmp = NULL;
    s_rm.capOld = NULL;
    s_rm.paintDC = NULL;
    s_rm.paintBmp = NULL;
    s_rm.paintOld = NULL;
    s_rm.capW = s_rm.capH = 0;
}

static BOOL CaptureFrame(void)
{
    HWND cap = s_rm.capture;
    BOOL ok = FALSE;
    HDC src;

    if (!cap || !IsWindow(cap)) return FALSE;
    FrameLock();
    if (s_paintDepth > 0) { FrameUnlock(); return FALSE; }  /* 目标正在自己画, 别抢位图 */
    if (!EnsureDib()) { FrameUnlock(); return FALSE; }

    /* 子窗口(远程画面)通常是 GDI 直绘, 先 BitBlt 最快最稳;
       顶层窗口则先用 PW_RENDERFULLCONTENT, 让 DWM 帮忙合成 */
    if (cap != s_rm.target) {
        src = GetDC(cap);
        if (src) {
            ok = BitBlt(s_rm.capDC, 0, 0, s_rm.capW, s_rm.capH, src, 0, 0, SRCCOPY);
            ReleaseDC(cap, src);
        }
    }
    if (!ok) {
        ok = PrintWindow(cap, s_rm.capDC,
                         PW_CLIENTONLY_ | PW_RENDERFULLCONTENT_);
    }
    if (!ok) {
        ok = PrintWindow(cap, s_rm.capDC, PW_CLIENTONLY_);
    }
    if (!ok) {
        src = GetDC(cap);
        if (src) {
            ok = BitBlt(s_rm.capDC, 0, 0, s_rm.capW, s_rm.capH, src, 0, 0, SRCCOPY);
            ReleaseDC(cap, src);
        }
    }
    FrameUnlock();
    return ok;
}

/* ------------------------------------------------------------------ */
/* 查看器绘制 / 输入                                                   */
/* ------------------------------------------------------------------ */
static BOOL MapViewerPoint(POINT client, POINT* out)
{
    LONG x, y;
    if (!s_rm.dst.right || !s_rm.dst.bottom) return FALSE;
    if (client.x < s_rm.dst.left || client.x >= s_rm.dst.right ||
        client.y < s_rm.dst.top  || client.y >= s_rm.dst.bottom) return FALSE;

    x = (LONG)((LONGLONG)(client.x - s_rm.dst.left) * s_rm.cw /
               (s_rm.dst.right - s_rm.dst.left));
    y = (LONG)((LONGLONG)(client.y - s_rm.dst.top) * s_rm.ch /
               (s_rm.dst.bottom - s_rm.dst.top));
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= s_rm.cw) x = s_rm.cw - 1;
    if (y >= s_rm.ch) y = s_rm.ch - 1;
    out->x = x;
    out->y = y;
    return TRUE;
}

static void ForwardMouse(UINT msg, WPARAM wp, LPARAM lp)
{
    POINT p, cp;
    if (!s_rm.input || !IsWindow(s_rm.input)) return;

    p.x = GET_X_LPARAM(lp);
    p.y = GET_Y_LPARAM(lp);
    if (!MapViewerPoint(p, &cp)) return;      /* 黑边上不算, 只有画面区域有效 */

    if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) {
        POINT tp = cp;
        if (ClientToScreen(s_rm.input, &tp))
            PostMessageW(s_rm.input, msg, wp,
                         MAKELPARAM((short)tp.x, (short)tp.y));
    } else {
        PostMessageW(s_rm.input, msg, wp, MAKELPARAM((short)cp.x, (short)cp.y));
    }
}

static void PaintViewer(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT cr;
    HBRUSH black;
    HDC mem;

    GetClientRect(hwnd, &cr);
    if (cr.right < 1 || cr.bottom < 1) { EndPaint(hwnd, &ps); return; }

    /* 双缓冲: 黑底 + 画面先在内存里画好, 再一次贴上屏。
       分两次画在屏幕 DC 上时, 高速刷新下会先看到黑底那一瞬(一闪一闪)。 */
    if (s_rm.viewDC && (s_rm.viewW != cr.right || s_rm.viewH != cr.bottom)) {
        SelectObject(s_rm.viewDC, s_rm.viewOld);
        DeleteObject(s_rm.viewBmp);
        DeleteDC(s_rm.viewDC);
        s_rm.viewDC = NULL;
        s_rm.viewBmp = NULL;
    }
    if (!s_rm.viewDC) {
        s_rm.viewDC = CreateCompatibleDC(dc);
        s_rm.viewBmp = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
        s_rm.viewOld = (HBITMAP)SelectObject(s_rm.viewDC, s_rm.viewBmp);
        s_rm.viewW = cr.right;
        s_rm.viewH = cr.bottom;
    }
    mem = s_rm.viewDC ? s_rm.viewDC : dc;

    black = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(mem, &cr, black);
    DeleteObject(black);

    if (!s_rm.useDwm && s_rm.capBmp && !IsRectEmpty(&s_rm.dst)) {
        FrameLock();
        SetStretchBltMode(mem, HALFTONE);
        SetBrushOrgEx(mem, 0, 0, NULL);
        StretchBlt(mem,
                   s_rm.dst.left, s_rm.dst.top,
                   s_rm.dst.right - s_rm.dst.left,
                   s_rm.dst.bottom - s_rm.dst.top,
                   s_rm.capDC, 0, 0, s_rm.capW, s_rm.capH,
                   SRCCOPY);
        FrameUnlock();
    } else if (s_rm.cw < 1) {
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(150, 150, 150));
        DrawTextW(mem, L"等待源窗口画面...", -1, &cr,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    if (mem != dc)
        BitBlt(dc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK ViewerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        PaintViewer(hwnd);
        return 0;

    case WM_MOVE:
        /* 拖动查看器时控制面板要实时跟随(等拖放结束才动会明显滞后) */
        WctlPanelTick();
        return 0;

    case WM_SIZE:
        /* 同步模式下查看器尺寸变了(用户拉伸/全屏), 源窗口要立刻跟着变,
           不能等 300ms 的纠偏定时器, 否则这段时间画面会错位(球跑到右下角)。 */
        if (s_rm.active && s_rm.scaleMode == WCTL_REMAPSCALE_SYNC) {
            WndCtx* c = WctlFindCtx(s_rm.target);
            if (c) EnforceGeometry(c);
        }
        UpdateContentSize();
        UpdateLayout();
        InvalidateRect(hwnd, NULL, FALSE);
        WctlPanelTick();
        return 0;

    case WM_TIMER:
        if (wp == RM_TIMER && s_rm.active) {
            DWORD now = GetTickCount();
            UpdateContentSize();
            UpdateLayout();
            if (!s_rm.useDwm && !IsIconic(hwnd)) {
                /* 目标程序自己在画(绘制已被重定向到抓取位图)时不需要额外抓帧;
                   只有它一帧都没画过(纯 DirectX 等情况)才退回 PrintWindow。
                   一旦它画过, 就绝不再用 PrintWindow —— 屏幕外的窗口 PrintWindow
                   只能拿到旧快照, 拿它覆盖实时帧就是"一闪一闪"的根源。 */
                if (!s_everPainted && now - s_lastPaint >= 500) {
                    if (CaptureFrame()) {
                        s_rm.failCount = 0;
                        InvalidateRect(hwnd, NULL, FALSE);
                    } else {
                        s_rm.failCount++;
                        /* AUTO + GDI 连续抓不到: 换 DWM 缩略图试试 */
                        if (s_rm.mode == WCTL_REMAPCAP_AUTO && s_rm.failCount >= 20)
                            TryDwm();
                    }
                } else {
                    s_rm.failCount = 0;
                }
            }
        }
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        if (mmi) {
            mmi->ptMinTrackSize.x = 360;
            mmi->ptMinTrackSize.y = 260;
        }
        return 0;
    }

    /* ---- 鼠标: 只转发画面区域内的, 坐标反向换算 ---- */
    case WM_MOUSEMOVE:
        ForwardMouse(msg, wp, lp);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        SetFocus(hwnd);
        SetCapture(hwnd);
        ForwardMouse(msg, wp, lp);
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        ForwardMouse(msg, wp, lp);
        ReleaseCapture();
        return 0;
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDBLCLK:
        ForwardMouse(msg, wp, lp);
        return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        ForwardMouse(msg, wp, lp);
        return 0;

    /* ---- 输入法: 查看器拿到组字上下文, 把"最终结果字符串"拆成 WM_CHAR 转发 ----
       目标程序的输入法上下文是它自己窗口的, 转发 WM_IME_* 过去它也不认; 直接送
       字符消息最通用(编辑框/自绘输入框都能收到)。 */
    case WM_IME_COMPOSITION:
        if (lp & GCS_RESULTSTR) {
            HIMC himc = (HIMC)(ULONG_PTR)wp;
            LONG bytes = ImmGetCompString(himc, GCS_RESULTSTR, NULL, 0);
            if (bytes > 0) {
                wchar_t* buf = (wchar_t*)malloc((size_t)bytes + sizeof(wchar_t));
                if (buf) {
                    LONG i;
                    ImmGetCompString(himc, GCS_RESULTSTR, buf, (DWORD)bytes);
                    buf[bytes / (LONG)sizeof(wchar_t)] = 0;
                    if (s_rm.input && IsWindow(s_rm.input)) {
                        for (i = 0; i < bytes / (LONG)sizeof(wchar_t); i++)
                            PostMessageW(s_rm.input, WM_CHAR, (WPARAM)buf[i], 1);
                    }
                    free(buf);
                }
            }
        }
        return 0;
    case WM_IME_CHAR:
        /* 不走组字上下文的输入法会直接发 WM_IME_CHAR: 也转成字符消息 */
        if (s_rm.input && IsWindow(s_rm.input))
            PostMessageW(s_rm.input, WM_CHAR, wp, lp);
        return 0;

    /* ---- 键盘: 直接按消息转发给源窗口 ---- */
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        if (s_rm.input && IsWindow(s_rm.input))
            PostMessageW(s_rm.input, msg, wp, lp);
        return 0;
    case WM_CHAR:
    case WM_SYSCHAR:
        /* 目标线程会对转发过去的 WM_KEYDOWN 自己 TranslateMessage, 这里丢掉避免重复 */
        return 0;

    case WM_CLOSE:
        /* 关闭查看器 = 停止重映射(把源窗口搬回屏幕) */
        if (g_shm) InterlockedAnd((LONG volatile*)&g_shm->flags, ~WCTL_F_REMAP);
        WctlRemapEnd();
        WctlForceApply();
        return 0;

    case WM_DESTROY:
        if (hwnd == s_rm.viewer) {
            s_rm.viewer = NULL;
            if (!s_rm.ending && s_rm.active) {
                if (g_shm) InterlockedAnd((LONG volatile*)&g_shm->flags, ~WCTL_F_REMAP);
                WctlRemapEnd();
                WctlForceApply();
            }
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static BOOL EnsureViewerClass(void)
{
    WNDCLASSEXW wc;
    if (s_classReg) return TRUE;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc   = ViewerProc;
    wc.hInstance     = g_hinst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = RM_CLASS;
    if (RegisterClassExW(&wc)) {
        s_classReg = TRUE;
        return TRUE;
    }
    if (GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        s_classReg = TRUE;
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* 源窗口: 期望尺寸 / 离屏几何                                         */
/* ------------------------------------------------------------------ */
static void DesiredContentSize(WndCtx* c, LONG* cw, LONG* ch)
{
    MONITORINFO mi;

    if (s_rm.scaleMode == WCTL_REMAPSCALE_SYNC) {
        /* 同步: 源窗口的尺寸跟着查看器走。
           位置模拟会告诉目标"你的客户区就是查看器客户区", 所以源窗口本身也必须
           真的被摆成同样大小 —— 否则目标会按一个它并没有的尺寸去画(把内容画到
           窗口外面, 查看器里就看到画面偏到右下角)。
           查看器初始尺寸=原窗口尺寸, 所以平时等于"保持原尺寸"; 用户全屏/拉伸查看器
           时源窗口跟着变, 1:1 映射才不会错位。 */
        RECT cr;
        if (s_rm.viewer && IsWindow(s_rm.viewer) &&
            GetClientRect(s_rm.viewer, &cr) && cr.right > 0 && cr.bottom > 0) {
            *cw = cr.right;
            *ch = cr.bottom;
            return;
        }
        if (GetClientRect(c->hwnd, &cr) && cr.right > 0 && cr.bottom > 0) {
            *cw = cr.right;
            *ch = cr.bottom;
            return;
        }
    }

    if (c->contentFixed && c->contentW > 0 && c->contentH > 0) {
        *cw = c->contentW;
        *ch = c->contentH;
        return;
    }
    /* 已经接管之后以第一次算出的画布尺寸为准, 不要因为源窗口被搬到屏幕外
       就换了一个最近显示器、把画布尺寸也换掉 */
    if (s_rm.active && s_rm.cw > 0 && s_rm.ch > 0) {
        *cw = s_rm.cw;
        *ch = s_rm.ch;
        return;
    }
    WctlGetMonitorInfoFor(c->hwnd, g_shm ? (int)g_shm->monitor : -1, &mi);
    *cw = mi.rcMonitor.right - mi.rcMonitor.left;
    *ch = mi.rcMonitor.bottom - mi.rcMonitor.top;
}

static void EnforceGeometry(WndCtx* c)
{
    LONG cw = 0, ch = 0, frameW, frameH, winW, winH;
    LONG vx, vy, x, y;
    RECT wr, cr;

    if (!c || !IsWindow(c->hwnd)) return;

    if (IsIconic(c->hwnd)) ShowWindow(c->hwnd, SW_SHOWNOACTIVATE);
    if (IsZoomed(c->hwnd)) ShowWindow(c->hwnd, SW_RESTORE);

    /* 同步模式也走同一套几何: 内容尺寸 = 查看器客户区(见 DesiredContentSize),
       所以源窗口会跟着查看器一起变大变小, 始终保持 1:1。 */
    DesiredContentSize(c, &cw, &ch);
    if (cw < 1 || ch < 1) return;

    GetWindowRect(c->hwnd, &wr);
    GetClientRect(c->hwnd, &cr);
    frameW = (wr.right - wr.left) - cr.right;
    frameH = (wr.bottom - wr.top) - cr.bottom;
    if (frameW < 0) frameW = 0;
    if (frameH < 0) frameH = 0;
    winW = cw + frameW;
    winH = ch + frameH;

    /* 摆到所有显示器的左边之外(虚拟桌面最左再往左) */
    vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    x = vx - winW - 64;
    y = vy + 32;

    if (wr.left != x || wr.top != y ||
        (wr.right - wr.left) != winW || (wr.bottom - wr.top) != winH) {
        SetWindowPos(c->hwnd, NULL, x, y, winW, winH,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
}

/* ------------------------------------------------------------------ */
/* 启动 / 停止                                                         */
/* ------------------------------------------------------------------ */
static BOOL RemapStart(WndCtx* c)
{
    MONITORINFO mi;
    RECT work;
    LONG cw = 0, ch = 0;
    int vw, vh, outerW, outerH, x, y;
    wchar_t title[256];
    DWORD style;
    DWORD exstyle;

    if (!c || !IsWindow(c->hwnd) || !g_shm) return FALSE;
    memset(&s_rm, 0, sizeof(s_rm));
    s_rm.target = c->hwnd;
    s_rm.mode = g_shm->remapCapture;
    s_rm.scaleMode = (g_shm->remapScale == WCTL_REMAPSCALE_SYNC)
                   ? WCTL_REMAPSCALE_SYNC : WCTL_REMAPSCALE_FULL;
    s_everPainted = FALSE;
    s_lastInv = 0;
    s_rm.savedZoomed  = IsZoomed(c->hwnd);
    s_rm.savedIconic   = IsIconic(c->hwnd);
    s_rm.savedVisible  = IsWindowVisible(c->hwnd);

    /* 保存"正常状态"的几何/样式(若窗口正被我们强制全屏, normal* 才是原始值) */
    if (c->haveNormal) {
        s_rm.savedRect    = c->normalRect;
        s_rm.savedStyle   = c->normalStyle;
        s_rm.savedExStyle = c->normalExStyle;
    } else {
        GetWindowRect(c->hwnd, &s_rm.savedRect);
        s_rm.savedStyle   = GetWindowLongPtrW(c->hwnd, GWL_STYLE);
        s_rm.savedExStyle = GetWindowLongPtrW(c->hwnd, GWL_EXSTYLE);
    }

    /* 抓取/输入目标: 有内容视图子窗口就抓子窗口(画面最干净) */
    if (c->contentFixed && c->contentChild && IsWindow(c->contentChild)) {
        s_rm.capture = c->contentChild;
        s_rm.input   = c->contentChild;
    } else {
        s_rm.capture = c->hwnd;
        s_rm.input   = c->hwnd;
    }

    if (IsIconic(c->hwnd)) ShowWindow(c->hwnd, SW_SHOWNOACTIVATE);
    if (IsZoomed(c->hwnd)) ShowWindow(c->hwnd, SW_RESTORE);

    /* 先记下源窗口原来所在显示器(下一步它就要被搬到屏幕外了) */
    WctlGetMonitorInfoFor(c->hwnd, g_shm ? (int)g_shm->monitor : -1, &mi);
    work = mi.rcWork;
    s_rm.monRect = mi.rcMonitor;

    /* 源窗口: 按缩放规则离屏(全屏=拉成屏幕大小, 同步=保持原尺寸) */
    EnforceGeometry(c);
    UpdateContentSize();
    cw = s_rm.cw > 0 ? s_rm.cw : 640;
    ch = s_rm.ch > 0 ? s_rm.ch : 480;

    /* 查看器大小:
         同步 = 与原窗口一样大(用户看到的就是"原窗口");
         全屏 = 工作区中央约 70%, 保持画面比例 */
    if (s_rm.scaleMode == WCTL_REMAPSCALE_SYNC) {
        vw = s_rm.savedRect.right - s_rm.savedRect.left;
        vh = s_rm.savedRect.bottom - s_rm.savedRect.top;
        if (vw < 360) vw = 360;
        if (vh < 260) vh = 260;
    } else {
        vw = (work.right - work.left) * 7 / 10;
        vh = (work.bottom - work.top) * 7 / 10;
        if (vw < 360) vw = 360;
        if (vh < 260) vh = 260;
        if ((LONGLONG)vw * ch <= (LONGLONG)vh * cw) {
            vh = (int)((LONGLONG)ch * vw / cw);
        } else {
            vw = (int)((LONGLONG)cw * vh / ch);
        }
        if (vw < 360) vw = 360;
        if (vh < 260) vh = 260;
    }

    style   = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    exstyle = 0;
    if (!EnsureViewerClass()) {
        WctlLogAlways(L"显示重映射: 注册查看器窗口类失败, error=%lu", GetLastError());
        return FALSE;
    }

    if (s_rm.scaleMode == WCTL_REMAPSCALE_SYNC) {
        /* 同步: 查看器就是"原窗口大小", 位置沿用原窗口位置(收进工作区) */
        outerW = vw;
        outerH = vh;
        x = s_rm.savedRect.left;
        y = s_rm.savedRect.top;
        if (x < work.left) x = work.left;
        if (y < work.top) y = work.top;
        if (x + outerW > work.right) x = work.right - outerW;
        if (y + outerH > work.bottom) y = work.bottom - outerH;
    } else {
        RECT r = { 0, 0, vw, vh };
        AdjustWindowRectEx(&r, style, FALSE, exstyle);
        outerW = r.right - r.left;
        outerH = r.bottom - r.top;
        x = work.left + ((work.right - work.left) - outerW) / 2;
        y = work.top  + ((work.bottom - work.top) - outerH) / 2;
    }

    _snwprintf(title, 255, L"显示重映射 - %s (关闭本窗口 = 还原源窗口)", WctlExeName());
    title[255] = 0;

    s_rm.viewer = CreateWindowExW(exstyle, RM_CLASS, title, style,
                                  x, y, outerW, outerH,
                                  NULL, NULL, g_hinst, NULL);
    if (!s_rm.viewer) {
        WctlLogAlways(L"显示重映射: 创建查看器失败, error=%lu", GetLastError());
        /* 查看器没建起来: 立刻把源窗口放回原处, 别让它留在屏幕外 */
        if (IsWindow(c->hwnd)) {
            SetWindowLongPtrW(c->hwnd, GWL_STYLE, s_rm.savedStyle);
            SetWindowLongPtrW(c->hwnd, GWL_EXSTYLE, s_rm.savedExStyle);
            SetWindowPos(c->hwnd, NULL,
                         s_rm.savedRect.left, s_rm.savedRect.top,
                         s_rm.savedRect.right - s_rm.savedRect.left,
                         s_rm.savedRect.bottom - s_rm.savedRect.top,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                         SWP_FRAMECHANGED);
        }
        memset(&s_rm, 0, sizeof(s_rm));
        return FALSE;
    }

    s_rm.active = TRUE;
    s_rm.lastEnforce = GetTickCount();
    s_paintDepth = 0;
    s_paintOwner = 0;
    s_lastPaint = 0;              /* 先让定时器抓一帧兜底, 等目标自己画的帧到了再停 */
    ShowWindow(s_rm.viewer, SW_SHOWNOACTIVATE);
    UpdateWindow(s_rm.viewer);
    SetTimer(s_rm.viewer, RM_TIMER, RM_CAPTURE_MS, NULL);
    UpdateContentSize();
    UpdateLayout();

    if (s_rm.mode == WCTL_REMAPCAP_DWM) {
        if (!TryDwm()) {
            WctlLogAlways(L"显示重映射: DWM 缩略图不可用, 回退到 PrintWindow 抓取");
        }
    } else {
        WctlLogAlways(L"显示重映射: 使用 PrintWindow/BitBlt 抓取客户区画面");
    }

    WctlLogAlways(L"显示重映射已启用: 源 hwnd=%08X 内容 %ldx%ld 已移出屏幕, 查看器 hwnd=%08X",
                  (unsigned)(ULONG_PTR)c->hwnd, cw, ch,
                  (unsigned)(ULONG_PTR)s_rm.viewer);
    return TRUE;
}

void WctlRemapEnd(void)
{
    HWND viewer, target;
    BOOL had = s_rm.active;

    if (s_rm.ending) return;
    s_rm.ending = TRUE;
    s_rm.active = FALSE;         /* 立刻停止新的绘制重定向 */

    viewer = s_rm.viewer;
    target = s_rm.target;
    /* 缩略图先注销, 再销毁查看器 */
    if (s_rm.thumb && pDwmUnregister) {
        pDwmUnregister(s_rm.thumb);
        s_rm.thumb = NULL;
    }
    s_rm.thumb  = NULL;
    s_rm.viewer = NULL;          /* 先断链, 避免 DestroyWindow 的 WM_DESTROY 再进来 */

    if (viewer && IsWindow(viewer)) {
        KillTimer(viewer, RM_TIMER);
        DestroyWindow(viewer);
    }
    /* 源窗口那边可能正在画: 等这一帧画完再释放位图(否则目标会画到已删除的 DC 上)。
       绘制线程不需要帧锁就能画完, 所以这里短暂等它调用 PaintDone 即可。 */
    {
        DWORD waited = 0;
        FrameLock();
        while (s_paintDepth > 0 && waited < 500) {
            FrameUnlock();
            Sleep(1);
            waited++;
            FrameLock();
        }
        s_paintDepth = 0;
        s_paintOwner = 0;
        ReleaseDib();
        FrameUnlock();
    }
    if (s_rm.viewDC) {
        SelectObject(s_rm.viewDC, s_rm.viewOld);
        DeleteObject(s_rm.viewBmp);
        DeleteDC(s_rm.viewDC);
        s_rm.viewDC = NULL;
        s_rm.viewBmp = NULL;
        s_rm.viewW = s_rm.viewH = 0;
    }

    if (target && IsWindow(target)) {
        RECT cur;
        LONG_PTR curStyle = GetWindowLongPtrW(target, GWL_STYLE);
        LONG_PTR curEx    = GetWindowLongPtrW(target, GWL_EXSTYLE);
        BOOL restyled = FALSE;
        if (s_rm.savedStyle && curStyle != s_rm.savedStyle) {
            SetWindowLongPtrW(target, GWL_STYLE, s_rm.savedStyle);
            restyled = TRUE;
        }
        if (s_rm.savedExStyle != curEx) {
            SetWindowLongPtrW(target, GWL_EXSTYLE, s_rm.savedExStyle);
            restyled = TRUE;
        }
        GetWindowRect(target, &cur);
        if (cur.left != s_rm.savedRect.left || cur.top != s_rm.savedRect.top ||
            (cur.right - cur.left) != (s_rm.savedRect.right - s_rm.savedRect.left) ||
            (cur.bottom - cur.top) != (s_rm.savedRect.bottom - s_rm.savedRect.top) ||
            restyled) {
            SetWindowPos(target, NULL,
                         s_rm.savedRect.left, s_rm.savedRect.top,
                         s_rm.savedRect.right - s_rm.savedRect.left,
                         s_rm.savedRect.bottom - s_rm.savedRect.top,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                         (restyled ? SWP_FRAMECHANGED : 0));
        }
        if (s_rm.savedZoomed) {
            ShowWindow(target, SW_MAXIMIZE);
        } else if (s_rm.savedIconic) {
            ShowWindow(target, SW_MINIMIZE);
        } else if (s_rm.savedVisible) {
            ShowWindow(target, SW_SHOWNOACTIVATE);
        }
        PostMessageW(target, WCTL_WM_RESYNC, 0, 0);
    }

    memset(&s_rm, 0, sizeof(s_rm));
    if (had) WctlLogAlways(L"显示重映射已停止, 源窗口已还原到屏幕");
}

void WctlRemapShutdown(void)
{
    WctlRemapEnd();
    if (s_dwmDll) {
        FreeLibrary(s_dwmDll);
        s_dwmDll = NULL;
        pDwmRegister = NULL;
        pDwmUpdate = NULL;
        pDwmUnregister = NULL;
        s_dwmTried = FALSE;
    }
}

void WctlRemapTick(void)
{
    WndCtx* c;
    DWORD now;

    if (!s_rm.active) return;
    if (!g_shm || !(g_shm->flags & WCTL_F_REMAP)) {
        WctlRemapEnd();
        return;
    }
    if (!s_rm.target || !IsWindow(s_rm.target) ||
        !s_rm.viewer || !IsWindow(s_rm.viewer)) {
        WctlRemapEnd();
        return;
    }

    c = WctlFindCtx(s_rm.target);
    if (!c || !c->isPrimary) {
        WctlRemapEnd();
        return;
    }

    /* 目标可能是"内容不随窗口缩放"的类型(远程桌面/固定分辨率画面): 接管时还不像
       霸屏窗口没关系, 现在源窗口已经被我们摆成大窗口了, 每帧补判一次。判定成立后
       内容尺寸以子窗口客户区为准, 后面的几何纠偏就不会再把它拉成屏幕大小了。 */
    if (!c->contentFixed && WctlContentLooksFixed(s_rm.target, NULL)) {
        HWND cc = WctlFindContentChild(s_rm.target);
        RECT ccr;
        c->contentFixed = TRUE;
        c->contentChild = cc;
        if (cc && GetClientRect(cc, &ccr) && ccr.right > 0 && ccr.bottom > 0) {
            c->contentW = ccr.right;
            c->contentH = ccr.bottom;
        }
        WctlLogAlways(L"显示重映射: 补检测到内容视图子窗口 %08X (内容 %ldx%ld)",
                      (unsigned)(ULONG_PTR)cc, (long)c->contentW, (long)c->contentH);
    }

    /* 内容视图子窗口被目标重建了: 重新挂到新的子窗口上 */
    if (c->contentFixed) {
        HWND ch = WctlFindContentChild(s_rm.target);
        if (ch && ch != s_rm.capture) {
            s_rm.capture = ch;
            s_rm.input   = ch;
            s_everPainted = FALSE;
            s_lastPaint = 0;
            ReleaseDib();
            UpdateContentSize();
            UpdateLayout();
        }
    } else if (!s_rm.capture || !IsWindow(s_rm.capture)) {
        s_rm.capture = s_rm.target;
        s_rm.input   = s_rm.target;
        s_everPainted = FALSE;
        s_lastPaint = 0;
        ReleaseDib();
    }

    now = GetTickCount();
    if (now - s_rm.lastEnforce >= RM_ENFORCE_MS) {
        s_rm.lastEnforce = now;
        if (IsIconic(s_rm.target)) ShowWindow(s_rm.target, SW_SHOWNOACTIVATE);
        if (IsZoomed(s_rm.target)) ShowWindow(s_rm.target, SW_RESTORE);
        EnforceGeometry(c);
        UpdateContentSize();
        UpdateLayout();
    }
}

void WctlRemapSync(WndCtx* c)
{
    if (!g_shm || !(g_shm->flags & WCTL_F_REMAP)) {
        WctlRemapEnd();
        return;
    }
    if (!c || c->dead || !c->isPrimary || !IsWindow(c->hwnd)) {
        if (s_rm.active) WctlRemapEnd();
        return;
    }
    if (s_rm.active) {
        if (s_rm.target != c->hwnd || !s_rm.viewer || !IsWindow(s_rm.viewer))
            WctlRemapEnd();
        else
            return;
    }
    RemapStart(c);
}

BOOL WctlRemapActive(void) { return s_rm.active; }
HWND WctlRemapSource(void) { return s_rm.active ? s_rm.target : NULL; }
HWND WctlRemapViewer(void)
{
    return (s_rm.active && s_rm.viewer && IsWindow(s_rm.viewer)) ? s_rm.viewer : NULL;
}

BOOL WctlIsRemapViewer(HWND hwnd)
{
    wchar_t cls[64];
    if (!hwnd) return FALSE;
    if (s_rm.viewer && hwnd == s_rm.viewer) return TRUE;
    if (GetClassNameW(hwnd, cls, 64) && lstrcmpiW(cls, RM_CLASS) == 0) return TRUE;
    return FALSE;
}

/* 当前重映射的缩放规则(-1 = 未在重映射) */
LONG WctlRemapScaleMode(void)
{
    return s_rm.active ? s_rm.scaleMode : -1;
}

/* 窗口位置模拟: 目标询问窗口位置/大小时, 谎报"用户实际看到的样子"。默认开, 可关。 */
BOOL WctlRemapPosSimOn(void)
{
    return s_rm.active && g_shm && g_shm->remapPosSim != WCTL_POSSIM_OFF;
}

/* 给目标程序看的"窗口矩形 / 客户区矩形":
     同步 = 查看器在桌面上的真实矩形(用户看到的就是它);
     全屏 = 占满源显示器的矩形(虽然用户实际看到的是窗口化的查看器)。 */
BOOL WctlRemapSimRect(RECT* win, RECT* cli)
{
    if (!WctlRemapPosSimOn()) return FALSE;
    if (s_rm.scaleMode == WCTL_REMAPSCALE_SYNC) {
        if (!s_rm.viewer || !IsWindow(s_rm.viewer)) return FALSE;
        if (win && !GetWindowRect(s_rm.viewer, win)) return FALSE;
        if (cli && !GetClientRect(s_rm.viewer, cli)) return FALSE;
        return TRUE;
    }
    if (win) *win = s_rm.monRect;
    if (cli) {
        cli->left = 0;
        cli->top = 0;
        cli->right  = s_rm.cw > 0 ? s_rm.cw : (s_rm.monRect.right - s_rm.monRect.left);
        cli->bottom = s_rm.ch > 0 ? s_rm.ch : (s_rm.monRect.bottom - s_rm.monRect.top);
    }
    return TRUE;
}

/* 重映射期间, "置顶 / 全屏"这两个开关作用到查看器上(源窗口反正没人看得见) */
void WctlRemapApplyViewer(BOOL topmost, BOOL fullscreen)
{
    HWND v = s_rm.viewer;
    if (!s_rm.active || !v || !IsWindow(v)) return;

    if (fullscreen && !s_rm.vFullOn) {
        /* 全屏到"查看器当前所在的"那个显示器 */
        HMONITOR hm = MonitorFromWindow(v, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        RECT mon;
        mi.cbSize = sizeof(mi);
        mon = s_rm.monRect;
        if (hm && GetMonitorInfoW(hm, &mi)) mon = mi.rcMonitor;
        GetWindowRect(v, &s_rm.vSavedRect);
        s_rm.vSavedStyle = GetWindowLongPtrW(v, GWL_STYLE);
        SetWindowLongPtrW(v, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(v, NULL,
                     mon.left, mon.top,
                     mon.right - mon.left,
                     mon.bottom - mon.top,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        s_rm.vFullOn = TRUE;
    } else if (!fullscreen && s_rm.vFullOn) {
        s_rm.vFullOn = FALSE;
        SetWindowLongPtrW(v, GWL_STYLE, s_rm.vSavedStyle);
        SetWindowPos(v, NULL,
                     s_rm.vSavedRect.left, s_rm.vSavedRect.top,
                     s_rm.vSavedRect.right - s_rm.vSavedRect.left,
                     s_rm.vSavedRect.bottom - s_rm.vSavedRect.top,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    /* 置顶只在状态变化时动一次: 每帧都 SetWindowPos(HWND_TOPMOST) 会把查看器
       重新顶到置顶带最上面, 控制面板(被查看器拥有)就会被压在下面, 用户再也点不到
       "置顶"按钮。 */
    if (s_rm.vTop != (topmost ? 1 : 0)) {
        s_rm.vTop = topmost ? 1 : 0;
        SetWindowPos(v, topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
    UpdateLayout();
    InvalidateRect(v, NULL, FALSE);
}

/* ------------------------------------------------------------------ */
/* 源窗口绘制重定向                                                    */
/* ------------------------------------------------------------------ */
/* 源窗口在屏幕外时 DWM/PrintWindow 只能拿到"搬出去之前"的旧画面, 所以这里
   直接把目标程序的绘制(DC)换成我们的位图: 它画什么, 查看器就显示什么。
   目标画进 paintDC(独占), 画完在 PaintDone 里整帧拷到 capDC(查看器只读 capDC),
   这样既不会读到画了一半的帧, 也不需要把锁一直压在目标程序的绘制过程上。 */
BOOL WctlRemapPaintBegin(HWND hwnd, HDC* outDC, LONG* outW, LONG* outH)
{
    DWORD me = GetCurrentThreadId();
    BOOL ok = FALSE;

    if (!s_rm.active || s_rm.useDwm || !outDC) return FALSE;
    if (hwnd != s_rm.capture) return FALSE;

    FrameLock();
    if ((s_paintDepth > 0 && s_paintOwner != me) ||
        (s_paintDepth == 0 && !EnsureDib())) {
        FrameUnlock();                       /* 别的线程正在画 / 位图没准备好: 让给它 */
        return FALSE;
    }
    s_paintOwner = me;
    s_paintDepth++;
    *outDC = s_rm.paintDC;
    if (outW) *outW = s_rm.capW;
    if (outH) *outH = s_rm.capH;
    ok = TRUE;
    FrameUnlock();
    return ok;
}

void WctlRemapPaintDone(HWND hwnd)
{
    DWORD me = GetCurrentThreadId();
    HWND viewer;

    if (hwnd != s_rm.capture) return;
    if (!s_frameCsInit) return;
    FrameLock();
    if (s_paintDepth > 0 && s_paintOwner == me) {
        s_paintDepth--;
        if (s_paintDepth == 0) {
            DWORD now;
            s_paintOwner = 0;
            if (s_rm.capDC && s_rm.paintDC && s_rm.capW > 0 && s_rm.capH > 0)
                BitBlt(s_rm.capDC, 0, 0, s_rm.capW, s_rm.capH,
                       s_rm.paintDC, 0, 0, SRCCOPY);
            s_everPainted = TRUE;
            s_lastPaint = GetTickCount();
            now = s_lastPaint;
            viewer = s_rm.viewer;
            /* 限频重画(~33fps): 帧已拷好, 刷太勤只会闪和费 CPU */
            if (viewer && IsWindow(viewer) && now - s_lastInv >= 30) {
                s_lastInv = now;
                InvalidateRect(viewer, NULL, FALSE);
            }
        }
    }
    FrameUnlock();
}
