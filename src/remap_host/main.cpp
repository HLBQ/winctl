#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <stdio.h>
#include <wchar.h>

/* "不可缩放"模式下的内容画布尺寸 = 屏幕分辨率(内容永远按屏幕大小渲染,
   窗口缩小时画面被裁掉, 不会跟着缩放)。在 wWinMain 里按当前屏幕初始化。 */
static LONG CANVAS_W = 1280;
static LONG CANVAS_H = 720;
#define BALL_R    36

static HWND  g_hwnd = NULL;
static HWND  g_child = NULL;
static FILE* g_log  = NULL;
static LONG  g_tick = 0;
static DWORD g_lastLog = 0;

/* 小球: 客户区坐标, 点哪里就移到哪里, 也可以直接拖拽 */
static LONG  g_ballX = 0, g_ballY = 0;
static BOOL  g_ballInit = FALSE;
static BOOL  g_dragging = FALSE;
static POINT g_lastClick = { -1, -1 };

/* 左下角输入框(自绘, 映射到查看器里也看得见输入内容) */
static wchar_t g_input[256] = L"";
static int     g_inputLen = 0;

/* 右侧按钮: 自己置顶 / 自己全屏 / 无边框全屏 / 固定尺寸(不可缩放) / 聚焦请求 */
static BOOL     g_selfTopmost = FALSE;
static BOOL     g_selfFull = FALSE;
static BOOL     g_blFull = FALSE;
static BOOL     g_fixed = FALSE;
static BOOL     g_focusReq = FALSE;
static DWORD    g_lastFocusTry = 0;
static RECT     g_fullSavedRect;
static LONG_PTR g_fullSavedStyle;
static RECT     g_blSavedRect;
static LONG_PTR g_blSavedStyle;

static void Log(const char* fmt, ...)
{
    va_list ap;
    if (!g_log) return;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}

static BOOL IsTopmostNow(HWND h)
{
    return (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOPMOST) ? TRUE : FALSE;
}

static BOOL HitBall(int x, int y)
{
    LONG dx = x - g_ballX, dy = y - g_ballY;
    return (dx * dx + dy * dy) <= (BALL_R + 6) * (BALL_R + 6);
}

static void ButtonRects(int w, RECT out[5])
{
    int bw = 156, bh = 30, x = w - bw - 16, i;
    for (i = 0; i < 5; i++) {
        out[i].left   = x;
        out[i].right  = x + bw;
        out[i].top    = 16 + i * (bh + 8);
        out[i].bottom = out[i].top + bh;
    }
}

static void DrawButton(HDC dc, const RECT* r, const wchar_t* text, BOOL on)
{
    HBRUSH b = CreateSolidBrush(on ? RGB(50, 120, 190) : RGB(28, 42, 60));
    HBRUSH border = CreateSolidBrush(RGB(90, 140, 200));
    RECT t = *r;
    FillRect(dc, r, b);
    FrameRect(dc, r, border);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, on ? RGB(255, 255, 255) : RGB(200, 215, 230));
    t.top += 6;
    DrawTextW(dc, text, -1, &t, DT_CENTER | DT_SINGLELINE);
    DeleteObject(b);
    DeleteObject(border);
}

static void DrawInputBox(HDC dc, int w, int h)
{
    RECT box;
    HBRUSH fill, border;
    wchar_t line[300];

    box.left   = 12;
    box.bottom = h - 12;
    box.top    = box.bottom - 30;
    box.right  = 12 + 440;
    if (box.right > w - 12) box.right = w - 12;
    if (box.top < h / 2) return;

    fill = CreateSolidBrush(RGB(10, 18, 30));
    FillRect(dc, &box, fill);
    DeleteObject(fill);
    border = CreateSolidBrush(RGB(90, 140, 200));
    FrameRect(dc, &box, border);
    DeleteObject(border);

    _snwprintf(line, 299, L"输入: %s", g_input);
    line[299] = 0;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(220, 230, 245));
    TextOutW(dc, box.left + 8, box.top + 7, line, lstrlenW(line));
    {
        SIZE sz;
        LONG cx;
        GetTextExtentPoint32W(dc, line, lstrlenW(line), &sz);
        cx = box.left + 8 + sz.cx + 2;
        if (cx < box.right - 4)
            TextOutW(dc, cx, box.top + 6, L"|", 1);
    }
}

/* 画布: w x h 是"逻辑画面尺寸"(固定模式 = 1280x720, 普通模式 = 实际客户区) */
static void DrawCanvas(HDC dc, int w, int h)
{
    RECT rc = { 0, 0, w, h };
    HBRUSH bg = CreateSolidBrush(RGB(20, 34, 52));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);

    {
        HPEN pen = CreatePen(PS_DOT, 1, RGB(70, 100, 130));
        HGDIOBJ old = SelectObject(dc, pen);
        MoveToEx(dc, 0, 0, NULL);        LineTo(dc, w, h);
        MoveToEx(dc, w, 0, NULL);        LineTo(dc, 0, h);
        SelectObject(dc, old);
        DeleteObject(pen);
    }

    if (!g_ballInit) { g_ballX = w / 2; g_ballY = h / 2; g_ballInit = TRUE; }
    {
        HBRUSH c = CreateSolidBrush(RGB(240, 180, 40));
        HGDIOBJ old = SelectObject(dc, c);
        Ellipse(dc, g_ballX - BALL_R, g_ballY - BALL_R,
                    g_ballX + BALL_R, g_ballY + BALL_R);
        SelectObject(dc, old);
        DeleteObject(c);
    }

    SetTextColor(dc, RGB(240, 240, 240));
    {
        wchar_t t[200];
        _snwprintf(t, 199, L"host ticks=%ld canvas=%dx%d", g_tick, w, h);
        t[199] = 0;
        TextOutW(dc, 20, 20, t, lstrlenW(t));
        _snwprintf(t, 199, L"ball=(%ld,%ld) click=(%ld,%ld) drag=%d",
                   (long)g_ballX, (long)g_ballY,
                   (long)g_lastClick.x, (long)g_lastClick.y, (int)g_dragging);
        t[199] = 0;
        TextOutW(dc, 20, 44, t, lstrlenW(t));
        _snwprintf(t, 199, L"topmost=%d full=%d bfull=%d fixed=%d focusReq=%d",
                   (int)IsTopmostNow(g_hwnd), (int)g_selfFull, (int)g_blFull,
                   (int)g_fixed, (int)g_focusReq);
        t[199] = 0;
        TextOutW(dc, 20, 68, t, lstrlenW(t));
    }

    DrawInputBox(dc, w, h);

    {
        RECT br[5];
        ButtonRects(w, br);
        DrawButton(dc, &br[0], L"自己置顶", IsTopmostNow(g_hwnd));
        DrawButton(dc, &br[1], g_selfFull ? L"还原窗口" : L"自己全屏", g_selfFull);
        DrawButton(dc, &br[2], L"全屏无边框窗口", g_blFull);
        DrawButton(dc, &br[3], L"固定尺寸(不可缩放)", g_fixed);
        DrawButton(dc, &br[4], L"聚焦请求(每秒)", g_focusReq);
    }
}

static void ToggleSelfTopmost(HWND h)
{
    BOOL want = !IsTopmostNow(h);
    SetWindowPos(h, want ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    Log("request self topmost=%d -> now=%d\r\n", (int)want, (int)IsTopmostNow(h));
    if (h == g_hwnd) { g_selfTopmost = want; InvalidateRect(h, NULL, FALSE); }
}

static void ToggleBorderlessFull(HWND h);

static void ToggleSelfFull(HWND h)
{
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!g_selfFull) {
        if (g_blFull) ToggleBorderlessFull(h);   /* 先退出无边框全屏, 避免保存的状态互相套娃 */
        GetWindowRect(h, &g_fullSavedRect);
        g_fullSavedStyle = GetWindowLongPtrW(h, GWL_STYLE);
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        /* 必须保留 WS_CLIPCHILDREN: 固定模式下子窗口盖满客户区, 丢了这个标志位,
           父窗口 100ms 一次的黑底 FillRect 就会直接画到子窗口上, 两边来回重绘 = 闪烁 */
        SetWindowLongPtrW(h, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN);
        SetWindowPos(h, HWND_TOP,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOACTIVATE);
        g_selfFull = TRUE;
    } else {
        g_selfFull = FALSE;
        SetWindowLongPtrW(h, GWL_STYLE, g_fullSavedStyle);
        SetWindowPos(h, NULL,
                     g_fullSavedRect.left, g_fullSavedRect.top,
                     g_fullSavedRect.right - g_fullSavedRect.left,
                     g_fullSavedRect.bottom - g_fullSavedRect.top,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    Log("request self fullscreen=%d\r\n", (int)g_selfFull);
    InvalidateRect(h, NULL, TRUE);
}

/* 无边框全屏(窗口模式): 与"自己全屏"(WS_POPUP 独占式)不同, 这里保留普通窗口身份
   (还在任务栏、没有置顶、别的窗口可以压到它上面), 只是去掉标题栏和边框并把尺寸
   拉到铺满整个显示器(连任务栏一起盖住), 即游戏里常见的"无边框窗口全屏"。 */
static void ToggleBorderlessFull(HWND h)
{
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!g_blFull) {
        LONG_PTR st;
        if (g_selfFull) ToggleSelfFull(h);       /* 先退出另一种全屏 */
        GetWindowRect(h, &g_blSavedRect);
        g_blSavedStyle = GetWindowLongPtrW(h, GWL_STYLE);
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        st = (g_blSavedStyle & ~(WS_CAPTION | WS_THICKFRAME)) | WS_CLIPCHILDREN | WS_VISIBLE;
        SetWindowLongPtrW(h, GWL_STYLE, st);
        SetWindowPos(h, HWND_TOP,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOACTIVATE);
        g_blFull = TRUE;
    } else {
        g_blFull = FALSE;
        SetWindowLongPtrW(h, GWL_STYLE, g_blSavedStyle);
        SetWindowPos(h, NULL,
                     g_blSavedRect.left, g_blSavedRect.top,
                     g_blSavedRect.right - g_blSavedRect.left,
                     g_blSavedRect.bottom - g_blSavedRect.top,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    Log("request borderless fullscreen=%d\r\n", (int)g_blFull);
    InvalidateRect(h, NULL, TRUE);
}

static void ToggleFixed(HWND h);

static void HandleClick(HWND h, int x, int y, int w, int hgt)
{
    POINT pt;
    RECT br[5];
    pt.x = x; pt.y = y;
    ButtonRects(w, br);

    g_lastClick.x = x;
    g_lastClick.y = y;

    if (PtInRect(&br[0], pt)) { ToggleSelfTopmost(h); return; }
    if (PtInRect(&br[1], pt)) { ToggleSelfFull(h); return; }
    if (PtInRect(&br[2], pt)) { if (h == g_hwnd) ToggleBorderlessFull(h); return; }
    if (PtInRect(&br[3], pt)) { if (h == g_hwnd) ToggleFixed(h); return; }
    if (PtInRect(&br[4], pt)) {
        g_focusReq = !g_focusReq;
        g_lastFocusTry = 0;
        Log("focus request mode=%d\r\n", (int)g_focusReq);
        InvalidateRect(g_hwnd, NULL, FALSE);
        if (g_child) InvalidateRect(g_child, NULL, FALSE);
        return;
    }

    if (HitBall(x, y)) {
        g_dragging = TRUE;
        SetCapture(h);
        Log("drag start x=%d y=%d ball=(%ld,%ld)\r\n", x, y, (long)g_ballX, (long)g_ballY);
    } else {
        g_ballX = x;
        g_ballY = y;
        Log("click move x=%d y=%d -> ball=(%ld,%ld)\r\n", x, y, (long)g_ballX, (long)g_ballY);
    }
    InvalidateRect(h, NULL, FALSE);
}

static void HandleChar(WPARAM wp)
{
    if (wp == 8) {
        if (g_inputLen > 0) g_input[--g_inputLen] = 0;
    } else if (wp == 13) {
        Log("enter: input len=%d\r\n", g_inputLen);
    } else if (wp >= 32 && wp < 0x10000) {
        if (g_inputLen < 250) {
            g_input[g_inputLen++] = (wchar_t)wp;
            g_input[g_inputLen] = 0;
        }
    }
    Log("char wp=%u len=%d\r\n", (unsigned)wp, g_inputLen);
    InvalidateRect(g_hwnd, NULL, FALSE);
    if (g_child) InvalidateRect(g_child, NULL, FALSE);
}

LRESULT CALLBACK CanvasProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        /* 固定模式: 永远按 1280x720 画, 窗口比它小就自然被裁掉(模拟不可缩放的画面) */
        DrawCanvas(dc, CANVAS_W, CANVAS_H);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        HandleClick(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), CANVAS_W, CANVAS_H);
        return 0;
    case WM_MOUSEMOVE:
        if (g_dragging) {
            g_ballX = GET_X_LPARAM(lp);
            g_ballY = GET_Y_LPARAM(lp);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_dragging) {
            g_dragging = FALSE;
            ReleaseCapture();
            g_ballX = GET_X_LPARAM(lp);
            g_ballY = GET_Y_LPARAM(lp);
            Log("drag end -> ball=(%ld,%ld)\r\n", (long)g_ballX, (long)g_ballY);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_CHAR:
        HandleChar(wp);
        return 0;
    case WM_ERASEBKGND:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        g_ballX = rc.right / 2;
        g_ballY = rc.bottom / 2;
        g_ballInit = TRUE;
        Log("created client=%ldx%ld ball=(%ld,%ld)\r\n",
            (long)rc.right, (long)rc.bottom, (long)g_ballX, (long)g_ballY);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (g_fixed) {
            /* 固定模式下父窗口只是黑底 + 固定大小的内容子窗口 */
            HBRUSH bg = CreateSolidBrush(RGB(8, 10, 14));
            FillRect(dc, &rc, bg);
            DeleteObject(bg);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(160, 170, 180));
            {
                wchar_t t[200];
                _snwprintf(t, 199, L"固定尺寸模式: 内容固定 %dx%d, 窗口 %ldx%ld (超出部分被裁掉, 窗口仍可拖边框缩放)",
                           CANVAS_W, CANVAS_H, (long)rc.right, (long)rc.bottom);
                t[199] = 0;
                TextOutW(dc, 20, 20, t, lstrlenW(t));
            }
        } else {
            DrawCanvas(dc, rc.right, rc.bottom);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wp == 1) {
            DWORD now;
            g_tick++;
            InvalidateRect(hwnd, NULL, FALSE);
            if (g_child) InvalidateRect(g_child, NULL, FALSE);
            now = GetTickCount();
            /* 聚焦请求: 每秒检查一次, 不是焦点就立刻抢焦点 */
            if (g_focusReq && now - g_lastFocusTry >= 1000) {
                HWND fg = GetForegroundWindow();
                g_lastFocusTry = now;
                if (fg == hwnd) {
                    Log("focus request: already focused\r\n");
                } else {
                    HWND before = fg;
                    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE);
                    SetForegroundWindow(hwnd);
                    Log("focus request: fg %p -> %p (self=%p)\r\n",
                        (void*)before, (void*)GetForegroundWindow(), (void*)hwnd);
                }
            }
            if (now - g_lastLog >= 700) {
                RECT wr;
                GetWindowRect(hwnd, &wr);
                g_lastLog = now;
                Log("tick=%ld self=%p isFg=%d topmost=%d full=%d fixed=%d focusReq=%d win=%ldx%ld ball=(%ld,%ld)\r\n",
                    (long)g_tick, (void*)hwnd,
                    GetForegroundWindow() == hwnd ? 1 : 0,
                    (int)IsTopmostNow(hwnd), (int)g_selfFull, (int)g_fixed,
                    (int)g_focusReq,
                    (long)(wr.right - wr.left), (long)(wr.bottom - wr.top),
                    (long)g_ballX, (long)g_ballY);
            }
        }
        return 0;
    case WM_SIZE:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        RECT rc;
        if (g_fixed) return 0;              /* 固定模式下画面/按钮都在子窗口里 */
        GetClientRect(hwnd, &rc);
        HandleClick(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), rc.right, rc.bottom);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_dragging) {
            g_ballX = GET_X_LPARAM(lp);
            g_ballY = GET_Y_LPARAM(lp);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_dragging) {
            g_dragging = FALSE;
            ReleaseCapture();
            g_ballX = GET_X_LPARAM(lp);
            g_ballY = GET_Y_LPARAM(lp);
            Log("drag end -> ball=(%ld,%ld)\r\n", (long)g_ballX, (long)g_ballY);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_RBUTTONDOWN:
        Log("mouse down msg=%u x=%d y=%d\r\n", msg,
            (int)(short)LOWORD(lp), (int)(short)HIWORD(lp));
        return 0;
    case WM_CHAR:
        HandleChar(wp);
        return 0;
    case WM_KEYDOWN:
        Log("key down wp=%u lp=%ld\r\n", (unsigned)wp, (long)lp);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* 进入/退出"不可缩放"模式(内容不可缩放, 窗口本身仍然可以硬缩放):
   进入 = 保持原来的普通可缩放窗口(有标题栏、能拖边框), 在里面建一个固定 1280x720 的
          内容子窗口(位置 0,0)。窗口比内容小 -> 画面只露出左上角一块, 其余被裁掉;
          窗口放大 -> 内容还是 1280x720, 多出来的地方是黑底。
          工具的重映射应该能抓到完整的 1280x720 画面再缩放进查看器里。 */
static void ToggleFixed(HWND h)
{
    if (!g_fixed) {
        g_child = CreateWindowExW(0, L"HostCanvasCls", L"内容", WS_CHILD | WS_VISIBLE,
                                  0, 0, CANVAS_W, CANVAS_H,
                                  h, NULL, GetModuleHandleW(NULL), NULL);
        g_fixed = TRUE;
        Log("fixed mode ON: child=%p canvas=%dx%d (窗口可继续拖边框缩放)\r\n",
            (void*)g_child, CANVAS_W, CANVAS_H);
    } else {
        g_fixed = FALSE;
        if (g_child) { DestroyWindow(g_child); g_child = NULL; }
        g_ballInit = FALSE;
        Log("fixed mode OFF\r\n");
    }
    InvalidateRect(h, NULL, TRUE);
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE prev, LPWSTR cmd, int show)
{
    wchar_t tmp[MAX_PATH], path[MAX_PATH];
    WNDCLASSEXW wc;
    MSG msg;
    HMODULE dll;
    typedef DWORD (WINAPI *PFN_ATTACH)(LPVOID);
    PFN_ATTACH attach;

    (void)prev; (void)cmd; (void)show;
    CANVAS_W = GetSystemMetrics(SM_CXSCREEN);
    CANVAS_H = GetSystemMetrics(SM_CYSCREEN);
    GetTempPathW(MAX_PATH, tmp);
    _snwprintf(path, MAX_PATH, L"%sremap_host.log", tmp);
    g_log = _wfopen(path, L"w");

    dll = LoadLibraryW(L"D:\\c++ project\\winctl\\bin\\winctl_hook.dll");
    if (!dll) { Log("LoadLibrary failed %lu\r\n", GetLastError()); return 1; }
    attach = (PFN_ATTACH)(void*)GetProcAddress(dll, "WctlAttach");
    if (!attach) { Log("no WctlAttach\r\n"); return 2; }
    Log("WctlAttach -> %lu\r\n", (unsigned long)attach(NULL));

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"RemapHostWndW";
    RegisterClassExW(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = CanvasProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"HostCanvasCls";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, L"RemapHostWndW", L"Remap Host",
                             WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN,
                             60, 60, 900, 600, NULL, NULL, hinst, NULL);
    if (!g_hwnd) { Log("CreateWindow failed %lu\r\n", GetLastError()); return 3; }
    SetTimer(g_hwnd, 1, 100, NULL);
    Log("self=%p\r\n", (void*)g_hwnd);

    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
