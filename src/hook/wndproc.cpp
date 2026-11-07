/* wndproc.cpp -- 子类化窗口过程: 在目标进程内吞掉各类请求
 *
 * 拦截点:
 *   WM_WINDOWPOSCHANGING  置顶请求 / 移动请求 / 全屏请求
 *   WM_STYLECHANGING      通过 SetWindowLong 去标题栏或加 WS_EX_TOPMOST
 *   WM_MOVING             用户拖拽标题栏或系统发起的移动
 *   WM_NCCALCSIZE/NCPAINT 目标自绘掉标题栏时, 强制走系统默认绘制
 */
#include "wctl_core.h"

#define WCTL_FULLSTRIP (WS_CAPTION | WS_THICKFRAME)

/* 该次 SetWindowPos 是否会让窗口变成置顶
 *
 * 注意: 系统在投递 WM_WINDOWPOSCHANGING 时会把 HWND_TOPMOST / HWND_NOTOPMOST
 * 解析成真实的窗口句柄, 所以不能只看伪句柄, 还要看被插入位置前面的窗口是不是置顶窗。
 * 同时必须排除我们自己发起的 Z 序调整(否则"取消置顶"会被自己的规则吞掉)。
 */
static BOOL PosLooksTopmost(WndCtx* c, HWND insertAfter, UINT posFlags)
{
    if (c && c->selfZOrder > 0) return FALSE;
    if (posFlags & SWP_NOZORDER) return FALSE;
    /* 面板是目标窗口的 owner, 系统会把"目标请求置顶"解析成"插到面板下面",
       这里同样视作置顶请求。 */
    if (g_panel && insertAfter == g_panel) return TRUE;
    if (insertAfter == HWND_TOPMOST) return TRUE;
    if (insertAfter == HWND_NOTOPMOST || insertAfter == HWND_BOTTOM ||
        insertAfter == HWND_TOP || insertAfter == NULL) return FALSE;
    if (IsWindow(insertAfter)) {
        LONG_PTR ex = GetWindowLongPtrW(insertAfter, GWL_EXSTYLE);
        if (ex & WS_EX_TOPMOST) return TRUE;
    }
    return FALSE;
}

LRESULT CALLBACK WctlSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    WndCtx* c = WctlFindCtx(hwnd);
    WNDPROC orig;
    LONG flags;
    BOOL consumed = FALSE;
    BOOL remapPri = FALSE;

    if (!c || !c->orig) return DefWindowProcW(hwnd, msg, wp, lp);
    orig  = c->orig;
    flags = WctlFlags();
    remapPri = c->isPrimary && (flags & WCTL_F_REMAP) != 0 && WctlRemapActive();

    /* ------------- 焦点模拟: 让目标以为它一直是前台窗口 -------------
       程序除了调用 GetForegroundWindow/GetActiveWindow/GetFocus (IAT 钩子已处理),
       还会根据 WM_ACTIVATEAPP / WM_NCACTIVATE / WM_ACTIVATE / WM_KILLFOCUS 的状态改变
       自己的行为(暂停渲染、停止接收输入等)。这里把这些消息改写成"保持激活"。 */
    if (flags & WCTL_F_FOCUS) {
        switch (msg) {
        case WM_ACTIVATEAPP:
            wp = TRUE;
            break;
        case WM_NCACTIVATE:
            wp = TRUE;
            break;
        case WM_ACTIVATE:
            wp = MAKELONG(WA_ACTIVE, HIWORD(wp));
            break;
        case WM_KILLFOCUS:
            return 0;            /* 吞掉"失去焦点", 目标继续认为自己有焦点 */
        }
    }

    switch (msg) {

    /* ------------- 置顶 / 移动 / 全屏: 统一在 WM_WINDOWPOSCHANGING 拦截 ------------- */
    case WM_WINDOWPOSCHANGING: {
        WINDOWPOS* wpos = (WINDOWPOS*)lp;
        if (wpos) {
            /* (1) 置顶开关关闭 -> 吞掉所有置顶请求 */
            if (!(flags & WCTL_F_TOPMOST) &&
                PosLooksTopmost(c, wpos->hwndInsertAfter, wpos->flags)) {
                HWND origInsert = wpos->hwndInsertAfter;
                wpos->hwndInsertAfter = HWND_NOTOPMOST;
                wpos->flags |= SWP_NOZORDER;
                c->blockTopmost++;
                if (c->blockTopmost <= 5 || (c->blockTopmost % 20) == 0)
                    WctlLog(L"吞掉置顶请求 #%ld 原 insert=%08X hwnd=%08X", c->blockTopmost,
                            (unsigned)(ULONG_PTR)origInsert, (unsigned)(ULONG_PTR)hwnd);
                consumed = TRUE;
            }

            /* (2) 移动锁 -> 吞掉所有设置窗口位置请求
                   (若开了 USERMOVE, 用户自己拖拽期间放行, 拖完由主循环重新捕获锁定点)
                   重映射期间源窗口必须留在屏幕外, 位置锁让路(否则会把源窗口拽回来) */
            if (!remapPri && (flags & WCTL_F_LOCKMOVE) && c->lockValid &&
                !(wpos->flags & SWP_NOMOVE) &&
                !IsIconic(hwnd) && !IsZoomed(hwnd) &&
                !((flags & WCTL_F_USERMOVE) && c->userMoving)) {
                if (wpos->x != c->lockPos.x || wpos->y != c->lockPos.y) {
                    wpos->x = c->lockPos.x;
                    wpos->y = c->lockPos.y;
                    c->blockMove++;
                    consumed = TRUE;
                }
            }

            /* (2b) 大小锁 -> 吞掉所有改变窗口尺寸的请求(程序会自己"居中"也算位置, 这里管大小) */
            if (!remapPri && (flags & WCTL_F_LOCKSIZE) && c->lockSizeValid &&
                !(wpos->flags & SWP_NOSIZE) &&
                !IsIconic(hwnd) && !IsZoomed(hwnd) &&
                !((flags & WCTL_F_USERMOVE) && c->userMoving)) {
                if (wpos->cx != c->lockW || wpos->cy != c->lockH) {
                    wpos->cx = c->lockW;
                    wpos->cy = c->lockH;
                    c->blockMove++;
                    consumed = TRUE;
                }
            }

            /* (3) 全屏开关 (重映射期间源窗口被刻意放在屏幕外, 这里必须放行) */
            if (!remapPri && c->isPrimary && !IsIconic(hwnd) &&
                (!(wpos->flags & SWP_NOMOVE) || !(wpos->flags & SWP_NOSIZE))) {
                MONITORINFO mi;
                RECT proposed;
                int mon = g_shm ? (int)g_shm->monitor : -1;

                proposed.left   = wpos->x;
                proposed.top    = wpos->y;
                proposed.right  = wpos->x + wpos->cx;
                proposed.bottom = wpos->y + wpos->cy;
                WctlGetMonitorInfoFor(hwnd, mon, &mi);

                if (!(flags & WCTL_F_FULLSCREEN)) {
                    /* 全屏已取消: 吞掉"精确铺满显示器"的请求, 并拉回带标题栏的窗口 */
                    if (!IsZoomed(hwnd) &&
                        WctlRectEqualsRect(&proposed, &mi.rcMonitor, 4) &&
                        !WctlRectEqualsRect(&proposed, &mi.rcWork, 4)) {
                        wpos->flags |= (SWP_NOMOVE | SWP_NOSIZE);
                        c->blockFullscreen++;
                        consumed = TRUE;
                        PostMessageW(hwnd, WCTL_WM_RESTORE, 0, 0);
                    }
                } else {
                    /* 强制全屏中: 吞掉试图缩小或移出显示器的请求 */
                    if (c->fullForced && !WctlRectEqualsRect(&proposed, &mi.rcMonitor, 0)) {
                        wpos->x  = mi.rcMonitor.left;
                        wpos->y  = mi.rcMonitor.top;
                        wpos->cx = mi.rcMonitor.right - mi.rcMonitor.left;
                        wpos->cy = mi.rcMonitor.bottom - mi.rcMonitor.top;
                        c->blockFullscreen++;
                        consumed = TRUE;
                    }
                }
            }
        }
        break;
    }

    /* ------------- 窗口状态锁: 最小化 / 最大化 / 系统菜单的移动缩放 -------------
       锁定窗口状态时, 窗口要被彻底钉死: 用户点标题栏的最小化、最大化, 双击标题栏,
       系统菜单里的"移动/大小", 以及目标程序自己发的这些命令, 一律吞掉。
       (别的进程直接 ShowWindow 的拦不到, 由 WctlApplyAll 的对账按回来。) */
    case WM_SYSCOMMAND: {
        UINT sc = (UINT)(wp & 0xFFF0);
        BOOL freeze = (flags & WCTL_F_FREEZE) != 0;
        BOOL lockSize = (flags & WCTL_F_LOCKSIZE) != 0;
        BOOL block = FALSE;

        if (remapPri) {
            /* 重映射的源窗口必须保持"可见且不最小化", 否则 DWM/抓取都会断流 */
            block = (sc == SC_MINIMIZE || sc == SC_MAXIMIZE || sc == SC_RESTORE);
        } else if (freeze) {
            block = (sc == SC_MINIMIZE || sc == SC_MAXIMIZE || sc == SC_MOVE || sc == SC_SIZE);
        } else if (lockSize) {
            block = (sc == SC_MAXIMIZE);      /* 最大化 = 改大小, 大小锁照样管 */
        }
        if (block) {
            c->blockState++;
            if (c->blockState <= 5 || (c->blockState % 20) == 0)
                WctlLog(L"吞掉系统命令 0x%04X (%s) #%ld hwnd=%08X",
                        (unsigned)sc,
                        remapPri ? L"显示重映射中" : (freeze ? L"窗口状态已锁" : L"大小已锁"),
                        c->blockState, (unsigned)(ULONG_PTR)hwnd);
            return 0;
        }
        break;
    }

    /* ------------- 样式变更: 加/去标题栏, WS_EX_TOPMOST ------------- */
    case WM_STYLECHANGING: {
        STYLESTRUCT* ss = (STYLESTRUCT*)lp;
        int idx = (int)wp;      /* wParam 是 GWL_STYLE / GWL_EXSTYLE(负数), 用 int 比较才安全 */
        if (ss) {
            if (idx == GWL_EXSTYLE) {
                if (c->selfZOrder == 0 &&
                    !(flags & WCTL_F_TOPMOST) &&
                    (ss->styleNew & WS_EX_TOPMOST) && !(ss->styleOld & WS_EX_TOPMOST)) {
                    ss->styleNew &= ~WS_EX_TOPMOST;      /* 吞掉置顶请求 */
                    c->blockTopmost++;
                    consumed = TRUE;
                }
            } else if (idx == GWL_STYLE) {
                if (!(flags & WCTL_F_FULLSCREEN)) {
                    if ((ss->styleOld & WS_CAPTION) && !(ss->styleNew & WS_CAPTION)) {
                        ss->styleNew |= (ss->styleOld & WCTL_CAPTION_BITS);
                        if ((ss->styleNew & WS_CAPTION) != WS_CAPTION)
                            ss->styleNew |= WCTL_CAPTION_BITS;
                        c->blockFullscreen++;
                        consumed = TRUE;
                    }
                } else {
                    if (ss->styleNew & WCTL_FULLSTRIP) {
                        ss->styleNew &= ~WCTL_FULLSTRIP;
                        c->blockFullscreen++;
                        consumed = TRUE;
                    }
                }
                /* 窗口状态/大小被锁时, 不许变成"最大化"样式(WS_MAXIMIZE)。
                   常见的最大化路径已经在 WM_SYSCOMMAND 里吞掉了, 这里是兜底:
                   目标程序自己 SetWindowLong 加 WS_MAXIMIZE 也会被抹掉。 */
                if (((flags & WCTL_F_FREEZE) || (flags & WCTL_F_LOCKSIZE) || remapPri) &&
                    !(ss->styleOld & WS_MAXIMIZE) && (ss->styleNew & WS_MAXIMIZE)) {
                    ss->styleNew &= ~WS_MAXIMIZE;
                    c->blockState++;
                    WctlLog(L"抹掉最大化样式 (%s) hwnd=%08X",
                            remapPri ? L"显示重映射中" :
                            ((flags & WCTL_F_FREEZE) ? L"窗口状态已锁" : L"大小已锁"),
                            (unsigned)(ULONG_PTR)hwnd);
                    consumed = TRUE;
                }
            }
        }
        break;
    }

    case WM_STYLECHANGED:
        WctlPostApply();
        break;

    /* ------------- 移动锁 / 大小锁: 拖拽与系统移动缩放 ------------- */
    case WM_MOVING: {
        if (!remapPri && (flags & WCTL_F_LOCKMOVE) && c->lockValid && !IsZoomed(hwnd) &&
            !((flags & WCTL_F_USERMOVE) && c->userMoving)) {
            RECT* pr = (RECT*)lp;
            if (pr) {
                int w = pr->right - pr->left;
                int h = pr->bottom - pr->top;
                if (pr->left != c->lockPos.x || pr->top != c->lockPos.y) {
                    pr->left   = c->lockPos.x;
                    pr->top    = c->lockPos.y;
                    pr->right  = pr->left + w;
                    pr->bottom = pr->top + h;
                    c->blockMove++;
                    return TRUE;
                }
            }
        }
        break;
    }

    /* ------------- 自绘标题栏的目标: 强制系统绘制 ------------- */
    case WM_NCCALCSIZE:
        if (c->captionAdded && wp == TRUE)
            return DefWindowProcW(hwnd, msg, wp, lp);
        break;

    case WM_NCPAINT:
        if (c->captionAdded)
            return DefWindowProcW(hwnd, msg, wp, lp);
        break;

    /* 标题栏是我们补的: 即使目标的窗口过程把整窗都当成客户区(无边框程序常这么写,
       会导致补出来的标题栏点不动/拖不动), 也按系统规则给出标题栏与边框命中,
       这样窗口才能被自由拖动和缩放。 */
    case WM_NCHITTEST:
        if (c->captionAdded) {
            LRESULT r = DefWindowProcW(hwnd, msg, wp, lp);
            if (r != HTCLIENT) return r;
        }
        break;

    /* 大小锁: 用户/系统拖边框缩放时按回锁定尺寸 */
    case WM_SIZING: {
        if (!remapPri && (flags & WCTL_F_LOCKSIZE) && c->lockSizeValid && !IsZoomed(hwnd) &&
            !((flags & WCTL_F_USERMOVE) && c->userMoving)) {
            RECT* pr = (RECT*)lp;
            if (pr) {
                pr->right  = pr->left + c->lockW;
                pr->bottom = pr->top + c->lockH;
                c->blockMove++;
                return TRUE;
            }
        }
        break;
    }

    /* 用户开始/结束拖拽窗口(Win32 在进入移动-缩放循环时通知) */    case WM_ENTERSIZEMOVE:
        c->userMoving = TRUE;
        WctlLog(L"用户开始拖拽/缩放窗口 hwnd=%08X", (unsigned)(ULONG_PTR)hwnd);
        break;

    case WM_EXITSIZEMOVE:
        c->userMoving = FALSE;
        WctlLog(L"用户结束拖拽/缩放窗口 hwnd=%08X", (unsigned)(ULONG_PTR)hwnd);
        WctlPostApply();
        break;

    /* ------------- 窗口状态变化 -> 让主循环重新对账 ------------- */
    case WM_WINDOWPOSCHANGED:
        /* 兜底: 万一某次置顶请求的解析方式没被上面的规则认出来(实测在窗口被 IME/其它
           置顶窗口扰动时会发生), 这里在 z 序真正改变后立刻把置顶样式位摘掉,
           把"可见的置顶时间"压到 0, 而不是等主循环 50ms 后才发现。 */
        if (!(flags & WCTL_F_TOPMOST)) {
            LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            if (ex & WS_EX_TOPMOST) {
                SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex & ~WS_EX_TOPMOST);
                c->blockTopmost++;
                WctlNoteTopmostClear();
                WctlPanelDropTopmost();   /* 面板也别被带着留在置顶带里 */
                WctlLog(L"事后纠正置顶 hwnd=%08X", (unsigned)(ULONG_PTR)hwnd);
            }
        }
        WctlPostApply();
        break;

    case WM_SIZE:
    case WM_MOVE:
    case WM_SHOWWINDOW:
    case WM_ACTIVATE:
    case WCTL_WM_RESTORE:
        /* 位置模拟: 源窗口实际被搬到了屏幕外, 但目标程序问起"你在哪"时,
           要回答"用户实际看到的那个位置"(同步=查看器位置, 全屏=显示器原点) */
        if (msg == WM_MOVE && remapPri && WctlRemapPosSimOn()) {
            RECT sim;
            if (WctlRemapSimRect(&sim, NULL))
                return CallWindowProcW(orig, hwnd, msg, wp,
                                       MAKELPARAM((WORD)(short)sim.left, (WORD)(short)sim.top));
        }
        WctlPostApply();
        if (msg == WCTL_WM_RESTORE) return 0;
        break;

    case WM_NCDESTROY:
        WctlMarkDead(hwnd);
        break;

    default:
        break;
    }

    (void)consumed;
    return CallWindowProcW(orig, hwnd, msg, wp, lp);
}
