/*
    panel.cpp -- 控制面板: 贴在目标窗口客户区里的一条按钮栏
 */
#include "wctl_core.h"

#define WCTL_PANEL_CLASS  L"WctlPanelClsW"
#define WCTL_HK_BASE      0xB100
#define GET_X_LP(lp)      ((int)(short)LOWORD(lp))
#define GET_Y_LP(lp)      ((int)(short)HIWORD(lp))
#define WCTL_PANEL_REBUILD_MS 600   /* 两次重建之间的最小间隔(限流) */

#define IDX_TOP       0
#define IDX_FULL      1
#define IDX_LOCK      2
#define IDX_FREEZE    3
#define IDX_FOCUS     4
#define IDX_REMAP     5
#define IDX_SCALERULE 6
#define IDX_FRONT     7
#define IDX_WINDOW    8
#define IDX_PIN       9
#define IDX_QUIT      10
#define IDX_KILL      11

typedef struct PanelBtn {
    const wchar_t* label;
    const wchar_t* labelWide; /* 布局时按更宽的那个算, 避免文字被挤掉 */
    LONG           flag;      /* 对应状态位, 0 = 纯动作按钮 */
    LONG           cmd;
    RECT           rc;
    BOOL           visible;
    BOOL           on;
} PanelBtn;

static PanelBtn s_btns[12] = {
    { L"置顶",   NULL,         WCTL_F_TOPMOST,    WCTL_CMD_TOPMOST,    {0}, FALSE, FALSE },
    { L"全屏",   NULL,         WCTL_F_FULLSCREEN, WCTL_CMD_FULLSCREEN, {0}, FALSE, FALSE },
    { L"移动锁", NULL,         WCTL_F_LOCKMOVE,   WCTL_CMD_LOCKMOVE,   {0}, FALSE, FALSE },
    { L"锁状态", NULL,         WCTL_F_FREEZE,     WCTL_CMD_FREEZE,     {0}, FALSE, FALSE },
    { L"焦点",   L"焦点模拟",   WCTL_F_FOCUS,      WCTL_CMD_FOCUS,      {0}, FALSE, FALSE },
    { L"重映射", L"显示重映射", WCTL_F_REMAP,      WCTL_CMD_REMAP,      {0}, FALSE, FALSE },
    { L"缩放规则", L"缩放规则", 0,                WCTL_CMD_SCALERULE,  {0}, FALSE, FALSE },
    { L"跳至顶层", NULL,       0,                 WCTL_CMD_FRONTPUSH,  {0}, FALSE, FALSE },
    { L"窗口化", NULL,         0,                 WCTL_CMD_AUTOSIZE,   {0}, FALSE, FALSE },
    { L"常亮",   NULL,         WCTL_F_PANEL_PIN,  WCTL_CMD_PIN,        {0}, FALSE, FALSE },
    { L"退出",   NULL,         0,                 WCTL_CMD_DETACH,     {0}, FALSE, FALSE },
    { L"强关",   L"确认关闭?", 0,                 WCTL_CMD_KILL,       {0}, FALSE, FALSE },
};
static const int s_btnNum = 12;

/* 按钮在"当前状态"下是否应该出现:
   重映射期间源窗口被搬到屏幕外, "窗口化"对它没意义(藏起来),
   而"缩放规则"(同步/全屏)只在重映射时才用得上。 */
static BOOL BtnApplies(int i)
{
    BOOL rm = WctlRemapActive();
    if (i == IDX_WINDOW)    return !rm;
    if (i == IDX_FRONT)     return !rm;   /* 重映射时"跳至顶层"没意义(源窗口本来就在屏幕外) */
    if (i == IDX_SCALERULE) return rm;
    return TRUE;
}
static LONG s_killArmUntil = 0;   /* 强关按钮的"二次确认"截止时刻 */

static HFONT  s_font    = NULL;
static UINT   s_fontDpi = 0;
static int    s_hotIdx  = -1;
static int    s_pressIdx= -1;
static int    s_curAlpha= -1;
static BOOL   s_hkOn    = FALSE;
static LONG   s_hkVk[4] = {0,0,0,0};
/* 面板创建时所处的"上下文": owner 与目标的置顶状态。两者任一变化都要重建面板。 */
static HWND   s_builtOwner = NULL;
static LONG   s_builtTop   = -1;
static DWORD  s_lastBuild  = 0;

/* 面板挂在谁身上: 平时挂主窗口; 显示重映射期间主窗口被搬到屏幕外,
   面板必须改挂查看器窗口, 否则用户会连"重映射/焦点/退出"按钮都点不到。 */
static HWND PanelAnchor(void)
{
    HWND v = WctlRemapViewer();
    if (v && IsWindow(v)) return v;
    return g_primary;
}

/* ------------------------------------------------------------------ */
static HFONT PanelFont(UINT dpi)
{
    if (s_font && s_fontDpi == dpi) return s_font;
    if (s_font) { DeleteObject(s_font); s_font = NULL; }
    s_font = CreateFontW(-MulDiv(9, (int)dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
                         FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                         L"Microsoft YaHei UI");
    if (!s_font)
        s_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    s_fontDpi = dpi;
    return s_font;
}

/* 量出"标题 + 全部按钮"需要的宽度(已按 DPI 缩放), 与 LayoutPanel 的排布规则一致:
   LayoutPanel 在 x + w > cw - 右边距 时停手, 所以这里算出的宽度正好让所有按钮都放得下。 */
static int PanelMeasureWidth(UINT dpi)
{
    HDC dc = GetDC(NULL);
    HFONT old;
    SIZE  sz;
    int   pad = WctlScale(9, dpi);
    int   gap = WctlScale(6, dpi);
    int   x   = WctlScale(8, dpi);
    int   i, total;

    if (!dc) return WctlScale(240, dpi);
    old = (HFONT)SelectObject(dc, PanelFont(dpi));
    GetTextExtentPoint32W(dc, L"窗口控制", 4, &sz);
    x += sz.cx + WctlScale(12, dpi);
    for (i = 0; i < s_btnNum; i++) {
        int w;
        const wchar_t* measure = s_btns[i].labelWide ? s_btns[i].labelWide : s_btns[i].label;
        if (!BtnApplies(i)) continue;
        GetTextExtentPoint32W(dc, measure, lstrlenW(measure), &sz);
        w = sz.cx + pad * 2;
        if (w < WctlScale(38, dpi)) w = WctlScale(38, dpi);
        x += w + gap;
    }
    SelectObject(dc, old);
    ReleaseDC(NULL, dc);
    total = x - gap + WctlScale(6, dpi);      /* 末尾少一个间隙, 再加右边距 */
    return total;
}

static void LayoutPanel(HDC dc, int cw, int ch, UINT dpi)
{
    HFONT old = (HFONT)SelectObject(dc, PanelFont(dpi));
    SIZE  sz;
    int   pad = WctlScale(9, dpi);
    int   gap = WctlScale(6, dpi);
    int   x   = WctlScale(8, dpi);
    int   top = WctlScale(4, dpi);
    int   bot = ch - WctlScale(4, dpi);
    int   i;
    int   titleW;

    GetTextExtentPoint32W(dc, L"窗口控制", 4, &sz);
    titleW = sz.cx;

    for (i = 0; i < s_btnNum; i++) s_btns[i].visible = FALSE;
    x += titleW + WctlScale(12, dpi);

    for (i = 0; i < s_btnNum; i++) {
        int w;
        const wchar_t* measure = s_btns[i].labelWide ? s_btns[i].labelWide : s_btns[i].label;
        if (!BtnApplies(i)) continue;              /* 保持 visible=FALSE */
        GetTextExtentPoint32W(dc, measure, lstrlenW(measure), &sz);
        w = sz.cx + pad * 2;
        if (w < WctlScale(38, dpi)) w = WctlScale(38, dpi);
        if (x + w > cw - WctlScale(6, dpi)) break;
        s_btns[i].rc.left = x;
        s_btns[i].rc.top = top;
        s_btns[i].rc.right = x + w;
        s_btns[i].rc.bottom = bot;
        s_btns[i].visible = TRUE;
        x += w + gap;
    }
    SelectObject(dc, old);
}

static int HitTestBtn(POINT ptScreen)
{
    RECT wr;
    POINT pt = ptScreen;
    if (!g_panel || !GetWindowRect(g_panel, &wr)) return -1;
    if (!PtInRect(&wr, pt)) return -1;
    pt.x -= wr.left;
    pt.y -= wr.top;
    for (int i = 0; i < s_btnNum; i++) {
        if (s_btns[i].visible && PtInRect(&s_btns[i].rc, pt)) return i;
    }
    return -1;
}

static void FillSolid(HDC dc, const RECT* rc, COLORREF col)
{
    HBRUSH b = CreateSolidBrush(col);
    FillRect(dc, rc, b);
    DeleteObject(b);
}

static void DrawRoundBtn(HDC dc, const RECT* rc, COLORREF bg, const wchar_t* text, BOOL pressed)
{
    HBRUSH br = CreateSolidBrush(pressed ? RGB(
                     (GetRValue(bg) > 20) ? GetRValue(bg) - 18 : 0,
                     (GetGValue(bg) > 20) ? GetGValue(bg) - 18 : 0,
                     (GetBValue(bg) > 20) ? GetBValue(bg) - 18 : 0) : bg);
    HPEN   pn = CreatePen(PS_SOLID, 1, RGB(
                     (GetRValue(bg) + 34 > 255) ? 255 : GetRValue(bg) + 34,
                     (GetGValue(bg) + 34 > 255) ? 255 : GetGValue(bg) + 34,
                     (GetBValue(bg) + 34 > 255) ? 255 : GetBValue(bg) + 34));
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, pn);
    RoundRect(dc, rc->left, rc->top, rc->right, rc->bottom, 6, 6);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pn);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(235, 238, 244));
    {
        RECT t = *rc;
        DrawTextW(dc, text, -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
}

static void PaintPanel(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    HDC mem;
    HBITMAP bmp, obmp;
    UINT dpi;
    LONG f = WctlFlags();
    int i;

    GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) { EndPaint(hwnd, &ps); return; }
    dpi = WctlDpiOf(hwnd);

    mem  = CreateCompatibleDC(dc);
    bmp  = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    obmp = (HBITMAP)SelectObject(mem, bmp);

    FillSolid(mem, &rc, RGB(24, 26, 31));
    {
        RECT line = rc;
        line.top = rc.bottom - 1;
        FillSolid(mem, &line, RGB(70, 78, 92));
    }

    /* 左侧标题 */
    {
        RECT t = rc;
        HFONT of = (HFONT)SelectObject(mem, PanelFont(dpi));
        t.left += WctlScale(8, dpi);
        t.right = t.left + WctlScale(90, dpi);
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(150, 158, 172));
        DrawTextW(mem, L"窗口控制", -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(mem, of);
    }

    LayoutPanel(mem, rc.right, rc.bottom, dpi);

    for (i = 0; i < s_btnNum; i++) {
        COLORREF bg;
        BOOL hot;
        const wchar_t* text;
        if (!s_btns[i].visible) continue;
        hot = (i == s_hotIdx);
        s_btns[i].on = (s_btns[i].flag != 0) && ((f & s_btns[i].flag) != 0);
        text = s_btns[i].label;

        if (s_btns[i].cmd == WCTL_CMD_DETACH) {
            bg = hot ? RGB(158, 68, 68) : RGB(122, 52, 52);
        } else if (s_btns[i].cmd == WCTL_CMD_KILL) {
            if ((LONG)GetTickCount() < s_killArmUntil) text = L"确认关闭?";
            bg = ((LONG)GetTickCount() < s_killArmUntil)
                     ? RGB(210, 60, 60)
                     : (hot ? RGB(178, 74, 74) : RGB(140, 58, 58));
        } else if (s_btns[i].cmd == WCTL_CMD_SCALERULE) {
            /* 缩放规则按钮直接显示当前模式: 同步 = 映射窗口 = 原窗口大小; 全屏 = 强制拉伸 */
            BOOL sync = g_shm && (g_shm->remapScale == WCTL_REMAPSCALE_SYNC);
            text = sync ? L"同步" : L"全屏";
            bg = sync ? (hot ? RGB(52, 158, 96) : RGB(38, 122, 72))
                      : (hot ? RGB(82, 90, 106) : RGB(52, 57, 68));
        } else if (s_btns[i].on) {
            bg = hot ? RGB(52, 158, 96) : RGB(38, 122, 72);
        } else {
            bg = hot ? RGB(82, 90, 106) : RGB(52, 57, 68);
        }
        DrawRoundBtn(mem, &s_btns[i].rc, bg, text, hot || s_pressIdx == i);
    }

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, obmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

/* 窗口状态已锁定时, 会跟"锁"打架的面板操作直接忽略(想用就先点一下"锁状态"解除)。
   GUI 里的同名开关在锁定时也是灰的, 这里保持一致。 */
static BOOL BlockedByFreeze(int cmd)
{
    if (!g_shm) return FALSE;
    if (!(g_shm->flags & WCTL_F_FREEZE)) return FALSE;
    return (cmd == WCTL_CMD_TOPMOST || cmd == WCTL_CMD_FULLSCREEN ||
            cmd == WCTL_CMD_LOCKMOVE || cmd == WCTL_CMD_AUTOSIZE) ? TRUE : FALSE;
}

/* ------------------------------------------------------------------ */
static void DoButton(int idx)
{
    if (idx < 0 || idx >= s_btnNum) return;
    if (BlockedByFreeze(s_btns[idx].cmd)) {
        WctlLogAlways(L"wctl: 窗口状态已锁定, 忽略面板命令 %s (先点\"锁状态\"解除)",
                      s_btns[idx].label);
        return;
    }
    switch (s_btns[idx].cmd) {
    case WCTL_CMD_DETACH:
        if (g_shm) g_shm->detach = 1;
        WctlForceApply();
        break;
    case WCTL_CMD_AUTOSIZE:
        /* 一键"窗口化": 取消全屏 + 解除移动锁 + 按自动尺寸重摆 + 保证标题栏可拖拽 */
        if (g_shm) {
            InterlockedAnd((LONG volatile*)&g_shm->flags,
                           ~(WCTL_F_FULLSCREEN | WCTL_F_LOCKMOVE));
            InterlockedExchange((LONG volatile*)&g_shm->lastCmd, WCTL_CMD_AUTOSIZE);
            InterlockedIncrement((LONG volatile*)&g_shm->resizeReq);
        }
        WctlForceApply();
        break;
    case WCTL_CMD_SCALERULE:
        /* 缩放规则: 同步(映射窗口=原窗口大小) <-> 全屏(强制拉伸再映射) */
        if (g_shm) {
            g_shm->remapScale = (g_shm->remapScale == WCTL_REMAPSCALE_SYNC)
                              ? WCTL_REMAPSCALE_FULL : WCTL_REMAPSCALE_SYNC;
        }
        WctlForceApply();
        break;
    case WCTL_CMD_FRONTPUSH:
        /* 一次性把目标窗口跳到最前(结束仍是普通窗口), 详见 WctlFrontPush。 */
        {
            WndCtx* c = g_primary ? WctlFindCtx(g_primary) : NULL;
            if (c) WctlFrontPush(c);
            WctlLogAlways(L"wctl: 跳至顶层(一次性) hwnd=%08X",
                          (unsigned)(ULONG_PTR)g_primary);
        }
        break;
    case WCTL_CMD_KILL:
        if ((LONG)GetTickCount() < s_killArmUntil) {
            s_killArmUntil = 0;
            if (g_shm) InterlockedIncrement((LONG volatile*)&g_shm->killReq);
            WctlForceApply();
        } else {
            s_killArmUntil = (LONG)(GetTickCount() + 3000);   /* 3 秒内再点一次才真关 */
        }
        break;
    default:
        if (s_btns[idx].flag) WctlToggleFlag(s_btns[idx].flag, s_btns[idx].cmd);
        break;
    }
    if (g_panel) InvalidateRect(g_panel, NULL, FALSE);
}

static LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        PaintPanel(hwnd);
        return 0;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_NCHITTEST:
        return HTCLIENT;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(NULL, IDC_HAND));
            return TRUE;
        }
        break;

    case WM_LBUTTONDOWN: {
        POINT pt;
        pt.x = GET_X_LP(lp);
        pt.y = GET_Y_LP(lp);
        ClientToScreen(hwnd, &pt);
        s_pressIdx = HitTestBtn(pt);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT pt;
        int idx;
        pt.x = GET_X_LP(lp);
        pt.y = GET_Y_LP(lp);
        ClientToScreen(hwnd, &pt);
        idx = HitTestBtn(pt);
        if (idx >= 0 && idx == s_pressIdx) DoButton(idx);
        s_pressIdx = -1;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_RBUTTONUP:
        WctlToggleFlag(WCTL_F_PANEL_PIN, WCTL_CMD_PIN);
        return 0;

    case WM_DESTROY:
        if (hwnd == g_panel) g_panel = NULL;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
BOOL WctlIsPanel(HWND hwnd) { return (hwnd != NULL && hwnd == g_panel); }

BOOL WctlIsPanelClass(HWND hwnd)
{
    wchar_t cls[64];
    cls[0] = 0;
    if (!hwnd) return FALSE;
    GetClassNameW(hwnd, cls, 63);
    return lstrcmpiW(cls, WCTL_PANEL_CLASS) == 0;
}

BOOL WctlPanelCreate(void)
{
    WNDCLASSEXW wc;

    if (g_panel && IsWindow(g_panel)) return TRUE;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = PanelProc;
    wc.hInstance     = g_hinst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = WCTL_PANEL_CLASS;
    RegisterClassExW(&wc);

    /* 面板由目标窗口"拥有"(owner): 系统保证被拥有窗口始终位于拥有者之上,
       不需要我们手动算 Z 序(SetWindowPos 的 hWndInsertAfter 语义是"放到它下面",
       很容易写反)。副作用是目标调用 SetWindowPos(HWND_TOPMOST) 时系统会把它解析成
       "插到自己面板下面", 这一点由 WndProc 里对 insertAfter==g_panel 的判断显式拦截。

       注意: owner 必须是"创建时"就指定的 —— 被拥有窗口的置顶带归属在创建那一刻定下,
       之后再改 owner 也拉不回来(所以主窗口变化时我们重建面板, 而不是改 GWLP_HWNDPARENT)。 */
    g_panel = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        WCTL_PANEL_CLASS, L"wctl-panel", WS_POPUP,
        0, 0, 320, 30, PanelAnchor(), NULL, g_hinst, NULL);
    if (!g_panel) {
        WctlLogAlways(L"wctl: 创建控制面板失败 err=%lu", GetLastError());
        return FALSE;
    }
    s_builtOwner = PanelAnchor();
    s_builtTop   = (WctlFlags() & WCTL_F_TOPMOST) ? 1 : 0;
    s_lastBuild  = GetTickCount();
    s_hotIdx     = -1;
    s_pressIdx   = -1;
    s_curAlpha   = g_shm ? (int)g_shm->panelAlpha : 0;
    SetLayeredWindowAttributes(g_panel, 0, (BYTE)s_curAlpha, LWA_ALPHA);
    WctlPanelUpdateHotkeys();
    WctlLog(L"控制面板已创建 hwnd=%08X owner=%08X topmost=%ld", (unsigned)(ULONG_PTR)g_panel,
            (unsigned)(ULONG_PTR)g_primary, (long)s_builtTop);
    return TRUE;
}

/* 只销毁窗口本身(不动热键/字体): 供"重建面板"使用 */
static void PanelDestroyWindow(void)
{
    HWND p = g_panel;
    if (!p || !IsWindow(p)) { g_panel = NULL; return; }
    g_panel = NULL;              /* 先清掉, PanelProc 的 WM_DESTROY 就不会再来动它 */
    DestroyWindow(p);
}

/* 重建面板: 先让目标处于最终置顶状态(WctlTick 已经保证), 再按当前 owner 新建窗口。
   这是"面板能不能压住已置顶目标"的关键, 详见文件头注释。 */
static void PanelRebuild(void)
{
    if (!PanelAnchor() || !IsWindow(PanelAnchor())) return;
    PanelDestroyWindow();
    WctlPanelCreate();
}

void WctlPanelDestroy(void)
{
    PanelDestroyWindow();
    for (int i = 0; i < 4; i++) UnregisterHotKey(NULL, WCTL_HK_BASE + i);
    s_hkOn = FALSE;
    if (s_font) { DeleteObject(s_font); s_font = NULL; s_fontDpi = 0; }
}

void WctlPanelInvalidate(void)
{
    if (g_panel && IsWindow(g_panel)) InvalidateRect(g_panel, NULL, FALSE);
}

/* 面板被"拥有者进入置顶带"带着一起置顶时, 立刻把它摘出来 */
void WctlPanelDropTopmost(void)
{
    LONG_PTR ex;
    if (!g_panel || !IsWindow(g_panel)) return;
    ex = GetWindowLongPtrW(g_panel, GWL_EXSTYLE);
    if (ex & WS_EX_TOPMOST) {
        SetWindowLongPtrW(g_panel, GWL_EXSTYLE, ex & ~WS_EX_TOPMOST);
        SetWindowPos(g_panel, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
}

void WctlPanelUpdateHotkeys(void)
{
    BOOL want = (WctlFlags() & WCTL_F_HOTKEYS) != 0;
    LONG vk[4];
    int  i;
    BOOL changed = (want != s_hkOn);

    vk[0] = g_shm ? g_shm->hotkeyTop   : 0;
    vk[1] = g_shm ? g_shm->hotkeyFull  : 0;
    vk[2] = g_shm ? g_shm->hotkeyLock  : 0;
    vk[3] = g_shm ? g_shm->hotkeyPanel : 0;
    for (i = 0; i < 4; i++) if (vk[i] != s_hkVk[i]) changed = TRUE;
    if (!changed) return;

    for (i = 0; i < 4; i++) UnregisterHotKey(NULL, WCTL_HK_BASE + i);
    s_hkOn = want;
    for (i = 0; i < 4; i++) {
        s_hkVk[i] = vk[i];
        if (want && vk[i]) {
            if (!RegisterHotKey(NULL, WCTL_HK_BASE + i,
                                MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, (UINT)vk[i]))
                WctlLog(L"热键注册失败 vk=%02X err=%lu", (unsigned)vk[i], GetLastError());
        }
    }
}

BOOL WctlPanelHotkey(WPARAM id)
{
    switch ((int)id - WCTL_HK_BASE) {
    case 0:
        if (BlockedByFreeze(WCTL_CMD_TOPMOST)) return TRUE;
        WctlToggleFlag(WCTL_F_TOPMOST, WCTL_CMD_TOPMOST); return TRUE;
    case 1:
        if (BlockedByFreeze(WCTL_CMD_FULLSCREEN)) return TRUE;
        WctlToggleFlag(WCTL_F_FULLSCREEN, WCTL_CMD_FULLSCREEN); return TRUE;
    case 2:
        if (BlockedByFreeze(WCTL_CMD_LOCKMOVE)) return TRUE;
        WctlToggleFlag(WCTL_F_LOCKMOVE, WCTL_CMD_LOCKMOVE); return TRUE;
    case 3: WctlToggleFlag(WCTL_F_PANEL, WCTL_CMD_PANEL); return TRUE;
    default: break;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* 面板是否压在目标之上(沿 Z 序链从上往下找, 谁先出现谁在上面) */
static BOOL PanelAboveTarget(HWND panel, HWND target)
{
    HWND cur = GetTopWindow(NULL);
    int  guard = 0;
    while (cur && guard++ < 5000) {
        if (cur == panel) return TRUE;
        if (cur == target) return FALSE;
        cur = GetWindow(cur, GW_HWNDNEXT);
    }
    return TRUE;    /* 链里没找到(目标已不在顶层链/最小化等): 不去折腾 */
}

void WctlPanelTick(void)
{
    HWND p = PanelAnchor();
    LONG f = WctlFlags();
    BOOL wantShow, wantTop, isTop;
    RECT cr, wr;
    POINT tl;
    UINT dpi;
    int h, w, x, y;
    LONG pos, vpos;
    LONG_PTR ex;
    POINT cur;
    BOOL hot;
    static HWND lastPrimary = NULL;

    WctlPanelUpdateHotkeys();

    /* "强关"二次确认超时 -> 恢复按钮文字 */
    if (s_killArmUntil && (LONG)GetTickCount() >= s_killArmUntil) {
        s_killArmUntil = 0;
        if (g_panel && IsWindow(g_panel)) InvalidateRect(g_panel, NULL, FALSE);
    }

    wantTop = (f & WCTL_F_TOPMOST) != 0;

    if ((!g_panel || !IsWindow(g_panel)) && (f & WCTL_F_PANEL) != 0)
        WctlPanelCreate();
    if (!g_panel || !IsWindow(g_panel)) return;

    /* 期望的置顶状态变了 / 主窗口变了 -> 重建面板。
       (WctlTick 已经先把目标摆成"期望状态", 所以此刻新建的面板会落在正确的层级里)
       另外: 万一面板还是被目标盖住(实测在外部程序抢置顶等情况下会出现), 也重建一次。 */
    if (p && IsWindow(p) &&
        (s_builtOwner != p || s_builtTop != (LONG)wantTop ||
         (wantTop && !PanelAboveTarget(g_panel, p)))) {
        if ((LONG)(GetTickCount() - s_lastBuild) >= WCTL_PANEL_REBUILD_MS) {
            WctlLog(L"重建控制面板 (owner %08X->%08X, 置顶 %ld->%ld)",
                    (unsigned)(ULONG_PTR)s_builtOwner, (unsigned)(ULONG_PTR)p,
                    (long)s_builtTop, (long)wantTop);
            PanelRebuild();
            if (!g_panel || !IsWindow(g_panel)) return;
        }
    }

    wantShow = (f & WCTL_F_PANEL) != 0;
    if (!p || !IsWindow(p) || !IsWindowVisible(p) || IsIconic(p)) wantShow = FALSE;
    if (!wantShow) {
        if (IsWindowVisible(g_panel)) ShowWindow(g_panel, SW_HIDE);
        return;
    }

    GetClientRect(p, &cr);
    tl.x = 0; tl.y = 0;
    ClientToScreen(p, &tl);
    dpi = WctlDpiOf(p);

    /* 高度: shm->panelHeight(逻辑像素), 不超过客户区高度 */
    h = WctlScale(g_shm ? (int)g_shm->panelHeight : 30, dpi);
    if (h < WctlScale(20, dpi)) h = WctlScale(20, dpi);
    if (cr.bottom > 0 && h > cr.bottom) h = cr.bottom;

    /* 宽度: panelWidth > 0 用固定逻辑宽度; = 0 时按"标题+全部按钮"的实际需要量出来,
       这样面板只占需要的那一点, 不再是一条占满整行的横条。 */
    if (g_shm && g_shm->panelWidth > 0) w = WctlScale((int)g_shm->panelWidth, dpi);
    else                                w = PanelMeasureWidth(dpi);
    if (w < WctlScale(120, dpi)) w = WctlScale(120, dpi);
    if (cr.right > 0 && w > cr.right) w = cr.right;
    if (w < 1) w = 1;

    /* 水平位置: 左 / 居中 / 右 / 自定义(panelX, 负数表示从右边算起) */
    pos = g_shm ? g_shm->panelPos : WCTL_PANELPOS_CENTER;
    switch (pos) {
    case WCTL_PANELPOS_LEFT:   x = 0; break;
    case WCTL_PANELPOS_RIGHT:  x = cr.right - w; break;
    case WCTL_PANELPOS_CUSTOM:
        x = g_shm ? WctlScale((int)g_shm->panelX, dpi) : 0;
        if (g_shm && g_shm->panelX < 0) x = cr.right - w + x;
        break;
    case WCTL_PANELPOS_CENTER:
    default:                   x = (cr.right - w) / 2; break;
    }
    if (x < 0) x = 0;
    if (x > cr.right - w) x = (cr.right - w > 0) ? (cr.right - w) : 0;

    /* 垂直位置: 顶 / 底 / 自定义(panelY, 负数表示从底边算起) */
    vpos = g_shm ? g_shm->panelVPos : WCTL_PANELVPOS_TOP;
    switch (vpos) {
    case WCTL_PANELVPOS_BOTTOM: y = cr.bottom - h; break;
    case WCTL_PANELVPOS_CUSTOM:
        y = g_shm ? WctlScale((int)g_shm->panelY, dpi) : 0;
        if (g_shm && g_shm->panelY < 0) y = cr.bottom - h + y;
        break;
    case WCTL_PANELVPOS_TOP:
    default:                    y = 0; break;
    }
    if (y < 0) y = 0;
    if (y > cr.bottom - h) y = (cr.bottom - h > 0) ? (cr.bottom - h) : 0;

    /* 面板只有"用户明确要求置顶"时才跟着置顶。
       不能跟着目标的置顶状态走: 那会形成自锁环——目标一被(外部)置顶 -> 面板也置顶 ->
       目标再请求置顶时被系统解析成"插到置顶的面板下面" -> 目标留在置顶带里取消不掉。
       平时面板靠 owner 关系就足以压在目标之上(全屏时同样可见)。
       下面是"重建之后"的兜底: 正常情况下重建已经把置顶带定好了, 这里只是补齐样式位。 */
    ex = GetWindowLongPtrW(g_panel, GWL_EXSTYLE);
    isTop = (ex & WS_EX_TOPMOST) != 0;
    if (wantTop != isTop) {
        if (!wantTop) {
            /* 附近存在别的置顶窗口(实测: 输入法状态条 SoPY_top 之类也是置顶窗)时,
               单靠 SetWindowPos(HWND_NOTOPMOST) 可能把面板留在置顶带里清不掉,
               所以直接把 WS_EX_TOPMOST 样式位摘掉, 再调整 Z 序。 */
            SetWindowLongPtrW(g_panel, GWL_EXSTYLE, ex & ~WS_EX_TOPMOST);
        }
        SetWindowPos(g_panel, wantTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    GetWindowRect(g_panel, &wr);
    {
        BOOL rectChanged = (wr.left != tl.x + x || wr.top != tl.y + y ||
                            wr.right - wr.left != w || wr.bottom - wr.top != h);
        /* Z 序由 owner 关系保证, 这里只负责位置尺寸与显示 */
        if (rectChanged || !IsWindowVisible(g_panel)) {
            SetWindowPos(g_panel, NULL, tl.x + x, tl.y + y, w, h,
                         SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
        }
    }
    if (p != lastPrimary) {
        lastPrimary = p;
        InvalidateRect(g_panel, NULL, FALSE);
    }

    /* 悬停 -> 变实 + 取消鼠标穿透。
       默认 panelAlpha = 0: 平时完全透明(看不见), 鼠标移到面板上才显示。 */
    GetCursorPos(&cur);
    GetWindowRect(g_panel, &wr);
    hot = PtInRect(&wr, cur);
    {
        int idx = hot ? HitTestBtn(cur) : -1;
        BOOL pin = (f & WCTL_F_PANEL_PIN) != 0;
        BOOL thruWanted = ((f & WCTL_F_CLICKTHRU) != 0) && !hot && !pin;
        int alpha = (hot || pin) ? (g_shm ? (int)g_shm->panelAlphaHot : 255)
                                 : (g_shm ? (int)g_shm->panelAlpha : 0);
        BOOL changed = FALSE;

        if (alpha < 0)   alpha = 0;
        if (alpha > 255) alpha = 255;
        if (alpha != s_curAlpha) {
            s_curAlpha = alpha;
            SetLayeredWindowAttributes(g_panel, 0, (BYTE)alpha, LWA_ALPHA);
            changed = TRUE;
        }
        ex = GetWindowLongPtrW(g_panel, GWL_EXSTYLE);
        if (thruWanted != (((ex & WS_EX_TRANSPARENT) != 0) ? TRUE : FALSE)) {
            /* 注意: 写 GWL_EXSTYLE 时必须带上原有的 WS_EX_TOPMOST, 否则会把置顶位抹掉 */
            SetWindowLongPtrW(g_panel, GWL_EXSTYLE,
                              thruWanted ? (ex | WS_EX_TRANSPARENT)
                                         : (ex & ~WS_EX_TRANSPARENT));
            changed = TRUE;
        }
        if (idx != s_hotIdx) { s_hotIdx = idx; changed = TRUE; }
        if (changed) InvalidateRect(g_panel, NULL, FALSE);
    }
}
