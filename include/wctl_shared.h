/* wctl_shared.h -- 注入器 CLI 与注入 DLL 之间的共享协议
 *
 * 设计要点:
 *   - 状态放在具名共享内存 (Local\wctl_shm_<pid>) 里, 注入器可以随时读写,
 *     注入 DLL 在目标进程内看到同一份数据。
 *   - flags  = 期望状态 (desired)  由注入器 / 面板按钮写入
 *   - actual = 实际状态 (applied)  由 DLL 写回, 注入器 --status 读取
 *   - cmdSeq 由注入器递增, DLL 处理完把 ackSeq 跟上, 用于确认命令生效
 *
 * 32/64 位兼容(重要):
 *   - 同一份共享内存可能被 64 位进程和 32 位进程同时映射, 所以结构体布局必须
 *     在两个位数下逐字节一致。这里只使用 4 字节对齐的成员(LONG / wchar_t),
 *     句柄这类指针大小的数据拆成 Lo/Hi 两个 LONG 存放 —— 否则 `long long` 在
 *     i386 上的默认对齐(4)与 x86-64(8)不同, 整个结构体会错位。
 *   - wchar_t 在 Windows 上恒为 2 字节(MSVC / MinGW 都是), 所以字符串部分天然一致。
 */
#ifndef WCTL_SHARED_H
#define WCTL_SHARED_H

#ifdef __cplusplus
extern "C" {
#endif

#define WCTL_SHM_MAGIC   0x4C544357u   /* 'WTCL' */
#define WCTL_SHM_VERSION 10

/* ---- flags / actual 位定义 ---- */
#define WCTL_F_TOPMOST      0x00000001  /* 置顶开关: 1=强制置顶, 0=吞掉所有置顶请求 */
#define WCTL_F_FULLSCREEN   0x00000002  /* 全屏开关: 1=强制全屏, 0=强制非全屏并保证标题栏 */
#define WCTL_F_LOCKMOVE     0x00000004  /* 移动锁:   1=吞掉所有设置窗口位置请求 */
#define WCTL_F_PANEL        0x00000008  /* 显示控制面板 */
#define WCTL_F_HOTKEYS      0x00000010  /* 注册全局热键 */
#define WCTL_F_CLICKTHRU    0x00000020  /* 面板空闲时鼠标穿透 */
#define WCTL_F_FORCE_NC     0x00000040  /* 强制标准非客户区(对抗自绘标题栏程序) */
#define WCTL_F_VERBOSE      0x00000080  /* 写日志 */
#define WCTL_F_PANEL_PIN    0x00000100  /* 面板常亮(不淡出) */
#define WCTL_F_USERMOVE     0x00000200  /* 移动锁的补充: 允许"用户手动拖拽", 只吞程序自己的移动请求 */
#define WCTL_F_LOCKSIZE     0x00000400  /* 锁定窗口大小: 吞掉所有改变尺寸的请求 */
#define WCTL_F_FREEZE       0x00000800  /* 锁定窗口状态: 置顶/位置/大小/全屏 全部按"冻结那一刻"固化 */
#define WCTL_F_ALLWINDOWS   0x00001000  /* 全屏/位置/大小 规则应用到进程内所有顶层窗口(多窗口程序) */
#define WCTL_F_KEEPCONTENT  0x00002000  /* 内容不随窗口缩放的目标(远程桌面/屏幕广播类): 不强行缩小以免裁切 */
#define WCTL_F_SCALECONTENT 0x00004000  /* 内容缩放: 缩小窗口并把远端画面等比缩放显示(不再裁掉画面) */
#define WCTL_F_FOCUS        0x00008000  /* 焦点模拟: 目标询问"我是不是前台"时回答"是"(并伪装激活消息) */
#define WCTL_F_REMAP        0x00010000  /* 显示重映射(第二种实现): 源窗口以原尺寸移出屏幕, 另开桌面窗口等比显示 + 输入穿透 */

#define WCTL_DEFAULT_FLAGS  (WCTL_F_PANEL | WCTL_F_HOTKEYS | WCTL_F_CLICKTHRU | WCTL_F_FORCE_NC | \
                             WCTL_F_KEEPCONTENT | WCTL_F_SCALECONTENT)

/* 面板水平/垂直位置 (shm->panelPos / shm->panelVPos) */
#define WCTL_PANELPOS_LEFT    0
#define WCTL_PANELPOS_CENTER  1
#define WCTL_PANELPOS_RIGHT   2
#define WCTL_PANELPOS_CUSTOM  3
#define WCTL_PANELVPOS_TOP    0
#define WCTL_PANELVPOS_BOTTOM 1
#define WCTL_PANELVPOS_CUSTOM 2

/* 面板按钮 / shm 中 lastCmd 的取值 */
#define WCTL_CMD_NONE       0
#define WCTL_CMD_TOPMOST    1
#define WCTL_CMD_FULLSCREEN 2
#define WCTL_CMD_LOCKMOVE   3
#define WCTL_CMD_PANEL      4
#define WCTL_CMD_DETACH     5
#define WCTL_CMD_PIN        6
#define WCTL_CMD_KILL       7   /* 强制关闭目标(先 WM_CLOSE, 超时后 TerminateProcess) */
#define WCTL_CMD_AUTOSIZE   8   /* 重新按"窗口化自动尺寸"摆放目标窗口 */
#define WCTL_CMD_FREEZE     9   /* 锁定窗口状态 */
#define WCTL_CMD_FOCUS      10  /* 焦点模拟开关 */
#define WCTL_CMD_REMAP      11  /* 显示重映射开关 */
#define WCTL_CMD_SCALERULE  12  /* 重映射缩放规则切换(同步/全屏) */
#define WCTL_CMD_FRONTPUSH  13  /* 一次性把目标窗口跳到最前(不动置顶状态) */

/* 重映射缩放规则 (shm->remapScale): 源窗口摆多大 */
#define WCTL_REMAPSCALE_FULL 0  /* 全屏: 源窗口强制拉成屏幕大小再映射(默认) */
#define WCTL_REMAPSCALE_SYNC 1  /* 同步: 源窗口保持原尺寸, 查看器窗口 = 原窗口大小 */

/* 重映射窗口位置模拟 (shm->remapPosSim): 目标询问窗口位置/大小时谎报"用户看到的样子" */
#define WCTL_POSSIM_AUTO 0      /* 默认 = 开 */
#define WCTL_POSSIM_ON   1
#define WCTL_POSSIM_OFF  2

/* 显示重映射的内容抓取方式 (shm->remapCapture) */
#define WCTL_REMAPCAP_AUTO  0   /* 先 GDI 抓取, 失败就退到 DWM 缩略图 */
#define WCTL_REMAPCAP_DWM   1   /* 强制使用 DWM 缩略图(源窗口离屏也能由 DWM 缩放合成) */
#define WCTL_REMAPCAP_GDI   2   /* 强制使用 PrintWindow/BitBlt 抓取到内存位图 */

#define WCTL_MAX_PATH 520

#pragma pack(push, 8)
typedef struct WctlShared {
    volatile LONG     magic;
    volatile LONG     version;
    volatile LONG     structSize;

    volatile LONG     cmdSeq;          /* 注入器 -> DLL 命令序号 */
    volatile LONG     ackSeq;          /* DLL -> 注入器 已处理序号 */
    volatile LONG     lastCmd;         /* WCTL_CMD_* */

    volatile LONG     flags;           /* 期望状态 */
    volatile LONG     actual;          /* 实际达成状态 */
    volatile LONG     detach;          /* 1 = 请求卸载全部功能并还原窗口 */
    volatile LONG     killReq;         /* >0 = 请求强制关闭目标进程 */
    volatile LONG     resizeReq;       /* >0 = 请求按窗口化自动尺寸重摆一次 */

    volatile LONG     pid;
    /* 注入器解析出的目标窗口句柄, DLL 优先使用。
       拆成两个 32 位字段: 这样 32 位与 64 位进程看到的偏移完全一致,
       也不会在 i386 上引入 8 字节对齐(以及 64 位原子操作)的依赖。 */
    volatile LONG     hwndTargetLo;    /* 句柄低 32 位 */
    volatile LONG     hwndTargetHi;    /* 句柄高 32 位(32 位进程恒为 0) */

    volatile LONG     ready;           /* DLL 初始化完成 */
    volatile LONG     lastError;       /* 最近一次错误 (Win32 错误码) */

    volatile LONG     lockX, lockY, lockValid;   /* 移动锁坐标 */
    volatile LONG     panelHeight;               /* 面板逻辑高度(96dpi 基准) */
    volatile LONG     panelAlpha;                /* 空闲不透明度 0-255 (默认 0: 肉眼完全不可见) */
    volatile LONG     panelAlphaHot;             /* 悬停不透明度 */
    volatile LONG     panelPos;                  /* 水平位置: WCTL_PANELPOS_* */
    volatile LONG     panelVPos;                 /* 垂直位置: WCTL_PANELVPOS_* */
    volatile LONG     panelWidth;                /* 0=按内容自适应宽度, >0=固定逻辑宽度 */
    volatile LONG     panelX, panelY;            /* pos/vpos = CUSTOM 时的逻辑坐标(客户区左上角起算) */
    volatile LONG     monitor;                   /* 目标显示器序号, -1=自动 */

    volatile LONG     hotkeyTop, hotkeyFull, hotkeyLock, hotkeyPanel; /* 虚拟键, 0=关闭 */
    volatile LONG     remapCapture;    /* WCTL_REMAPCAP_*: 显示重映射的抓取方式 */
    volatile LONG     remapScale;      /* WCTL_REMAPSCALE_*: 重映射缩放规则(同步/全屏) */
    volatile LONG     remapPosSim;     /* WCTL_POSSIM_*: 窗口位置模拟, 默认开 */

    wchar_t           status[256];     /* DLL 写回的可读状态 */
    wchar_t           dllPath[WCTL_MAX_PATH];
    wchar_t           targetExe[WCTL_MAX_PATH];
} WctlShared;
#pragma pack(pop)

/* 句柄字段的读写宏: 32 位与 64 位下都用它, 避免各处手写位移出错 */
#define WCTL_SHM_GET_HWND(s) \
    ((HWND)(ULONG_PTR)((((ULONG64)(DWORD)((s)->hwndTargetHi)) << 32) | \
                       ((ULONG64)(DWORD)((s)->hwndTargetLo))))
#define WCTL_SHM_SET_HWND(s, h) \
    do { ULONG64 wctl_h_ = (ULONG64)(ULONG_PTR)(h); \
         (s)->hwndTargetHi = (LONG)(DWORD)(wctl_h_ >> 32); \
         (s)->hwndTargetLo = (LONG)(DWORD)(wctl_h_ & 0xFFFFFFFFu); } while (0)

/* 共享内存 / 事件名字 */
#define WCTL_SHM_FMT   L"Local\\wctl_shm_%lu"
#define WCTL_EVT_FMT   L"Local\\wctl_evt_%lu"

#ifdef __cplusplus
}
#endif
#endif /* WCTL_SHARED_H */
