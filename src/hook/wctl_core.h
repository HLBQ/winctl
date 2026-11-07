/* wctl_core.h -- 注入 DLL 内部公共声明 */
#ifndef WCTL_CORE_H
#define WCTL_CORE_H

#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "wctl_shared.h"

#define WCTL_TICK_MS        50      /* 主循环周期 */
#define WCTL_TICK_FAST_MS   15      /* 侦测到"抢置顶很凶"时切到的快周期 */
#define WCTL_DISCOVER_EVERY 2       /* 每 N 个 tick 做一次窗口发现 */
#define WCTL_WM_RESTORE     (WM_APP + 1)
#define WCTL_WM_RESYNC      (WM_APP + 2)
#define WCTL_WM_APPLY       (WM_APP + 3)
#define WCTL_WM_DETACHED    (WM_APP + 4)

/* 需要"补齐"的标题栏样式位 */
#define WCTL_CAPTION_BITS   (WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME)

/* ------------------------------------------------------------------ */
/* 每个被接管的窗口上下文                                              */
/* ------------------------------------------------------------------ */
struct WndCtx {
    HWND        hwnd;
    WNDPROC     orig;
    BOOL        isPrimary;
    BOOL        dead;          /* WM_NCDESTROY 后置位, 由主线程延迟释放 */

    BOOL        haveNormal;    /* normal* 是否有效 */
    RECT        normalRect;
    LONG_PTR    normalStyle;
    LONG_PTR    normalExStyle;

    BOOL        fullForced;    /* 我们主动强制了全屏 */
    BOOL        captionAdded;  /* 标题栏是我们加上的 */

    BOOL        lockValid;
    POINT       lockPos;
    BOOL        lockSizeValid; /* 锁定大小 */
    LONG        lockW, lockH;
    LONG        lockEpoch;     /* 每次开启移动锁自增, 用于重新捕获锁定坐标 */
    BOOL        userMoving;    /* 用户正在拖拽/缩放(WM_ENTERSIZEMOVE 期间) */

    /* "内容不随窗口缩放"(远程桌面/屏幕广播类): 接管那一刻判定并记下内容尺寸,
       因为之后我们就会去改窗口大小/标题栏, 那时再判定已经晚了。 */
    BOOL        contentFixed;
    LONG        contentW, contentH;
    HWND        contentChild;    /* 真正画远端画面的那个子窗口(内容视图) */
    BOOL        scaleOn;         /* 这个窗口的内容正在被我们缩放显示 */
    BOOL        scaleWasOn;      /* 曾经缩放显示过(卸载时据此还原成"不裁画面"的尺寸) */

    LONG        selfZOrder;    /* >0 表示当前有一次"我们自己发起"的 Z 序请求在途 */

    LONG        blockTopmost;
    LONG        blockMove;
    LONG        blockFullscreen;
    LONG        blockState;     /* 吞掉的"最大化/最小化"次数(窗口状态锁) */

    /* 锁上那一刻窗口是不是正处于最大化/最小化: 是的话就别去"纠正"它,
       否则等于把用户当时的状态当成错误给强行改掉了。 */
    BOOL        zoomWasFrozen;
    BOOL        iconWasFrozen;

    WndCtx*     next;
};

/* ------------------------------------------------------------------ */
/* 全局                                                               */
/* ------------------------------------------------------------------ */
extern HINSTANCE        g_hinst;
extern WctlShared*      g_shm;
extern HANDLE           g_shmMap;
extern HANDLE           g_tickEvent;
extern HWND             g_panel;
extern HWND             g_primary;
extern CRITICAL_SECTION g_cs;
extern WndCtx*          g_ctxList;
extern volatile LONG    g_stop;
extern volatile LONG    g_detached;
extern DWORD            g_tickThreadId;

/* ---- 日志 / 状态 ---- */
void WctlLog(const wchar_t* fmt, ...);
void WctlLogAlways(const wchar_t* fmt, ...);
void WctlSetStatus(const wchar_t* fmt, ...);

/* ---- 状态位 ---- */
LONG WctlFlags(void);
void WctlForceApply(void);

/* ---- IPC ---- */
BOOL WctlIpcOpen(void);
void WctlIpcPoll(void);

/* ---- 窗口登记表 ---- */
WndCtx* WctlFindCtx(HWND hwnd);
WndCtx* WctlAddCtx(HWND hwnd);
void    WctlMarkDead(HWND hwnd);
void    WctlDiscover(void);
void    WctlUnsubclassAll(void);
void    WctlReapDead(void);
int     WctlCtxCount(void);

/* ---- 应用 ---- */
void WctlApplyAll(void);
void WctlPickPrimary(void);
void WctlSetZOrder(WndCtx* c, HWND insertAfter);
void WctlFrontPush(WndCtx* c);
void WctlSyncFromWindow(WndCtx* c);
void WctlRestoreWindow(WndCtx* c);
void WctlShutdown(void);
void WctlPostApply(void);
void WctlRequestKill(void);
void WctlAutoSize(void);
void WctlNoteTopmostClear(void);

/* ---- 子类过程 ---- */
LRESULT CALLBACK WctlSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

/* ---- 控制面板 ---- */
BOOL WctlPanelCreate(void);
void WctlPanelDestroy(void);
void WctlPanelTick(void);
void WctlPanelInvalidate(void);
void WctlPanelUpdateHotkeys(void);
BOOL WctlPanelHotkey(WPARAM id);
void WctlPanelDropTopmost(void);
BOOL WctlIsPanel(HWND hwnd);
BOOL WctlIsPanelClass(HWND hwnd);

/* ---- 工具 ---- */
BOOL  WctlRectCoversMonitor(const RECT* r, const RECT* mon, int tol);
BOOL  WctlRectEqualsRect(const RECT* a, const RECT* b, int tol);
BOOL  WctlIsFullscreenLike(HWND hwnd, RECT* outRect);
BOOL  WctlRectEmptyOrTiny(const RECT* r);
BOOL  WctlGetMonitorInfoFor(HWND hwnd, int monIdx, MONITORINFO* out);
UINT  WctlDpiOf(HWND hwnd);
int   WctlScale(int logical, UINT dpi);
RECT  WctlDefaultNormalRect(HWND hwnd);
void  WctlToggleFlag(LONG bit, LONG cmd);
const wchar_t* WctlExeName(void);
/* 目标内容是否"不随窗口缩放"(远程桌面/屏幕广播类: 有内容视图子窗口)
 * outChild: 可选的输出, 返回那个"内容视图子窗口"(真正画远端画面的子窗口) */
BOOL  WctlContentLooksFixed(HWND hwnd, HWND* outChild);
/* 在客户区里找"内容视图子窗口"(面积最大的、覆盖客户区六成以上的可见子窗口),
 * 不管窗口当前是不是霸屏状态 —— 缩放过程中窗口已经变小了也照找不误 */
HWND  WctlFindContentChild(HWND hwnd);
BOOL  WctlContentWasKept(void);

/* ---- 内容缩放 (远程画面类窗口) ---- */
BOOL WctlScaleInit(void);            /* 安装 IAT 挂钩(幂等) */
void WctlScaleShutdown(void);        /* 卸载挂钩 / 释放资源 */
void WctlScaleSync(WndCtx* c);       /* 按当前开关把这一个窗口的缩放对齐(可反复调用) */
void WctlScaleEnd(WndCtx* c);        /* 停止这个窗口的缩放 */
void WctlScaleEndByHwnd(HWND hwnd);  /* 窗口/内容子窗口被销毁时立刻收工(必须先于 WndCtx 释放) */
void WctlScaleStopAll(void);
void WctlScaleTick(void);            /* 主循环: 补挂新模块 / 健康检查 */
BOOL WctlScaleIsOn(WndCtx* c);

/* ---- 焦点模拟 ---- */
void WctlFocusTick(void);            /* 焦点模拟开关变化时向目标补发"已激活"消息 */

/* ---- 显示重映射 (第二种实现: 离屏源窗口 + 桌面查看器) ---- */
void WctlRemapSync(WndCtx* c);       /* 按当前开关把这一个主窗口的重映射对齐(可反复调用) */
void WctlRemapTick(void);            /* 主循环: 校验源窗口/查看器, 必要时重建 */
void WctlRemapEnd(void);             /* 停止重映射并把源窗口还原到屏幕 */
void WctlRemapShutdown(void);        /* 卸载时调用(停止 + 释放资源) */
BOOL WctlRemapActive(void);
HWND WctlRemapSource(void);
HWND WctlRemapViewer(void);
BOOL WctlIsRemapViewer(HWND hwnd);
/* 源窗口绘制重定向: 目标程序 BeginPaint/GetDC 时改画进重映射的抓取位图,
   这样即使源窗口在屏幕外(DWM 不给合成画面), 也能拿到实时内容。 */
BOOL WctlRemapPaintBegin(HWND hwnd, HDC* outDC, LONG* outW, LONG* outH);
void WctlRemapPaintDone(HWND hwnd);
/* 缩放规则/位置模拟/查看器置顶全屏 */
LONG WctlRemapScaleMode(void);
BOOL WctlRemapPosSimOn(void);
BOOL WctlRemapSimRect(RECT* win, RECT* cli);
void WctlRemapApplyViewer(BOOL topmost, BOOL fullscreen);

/* ---- 主循环 ---- */
void WctlTick(void);
void WctlRun(void);

#endif /* WCTL_CORE_H */
