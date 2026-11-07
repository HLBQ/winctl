/* main.cpp -- wctl 注入器 / 命令行控制台
 *
 * 用法概览:
 *   wctl [选项] <程序> [-- 程序参数...]     启动并注入
 *   wctl --attach <PID|进程名> [选项]        注入到已运行进程
 *   wctl --status <PID|进程名>
 *   wctl --set    <PID|进程名> topmost=on ...
 *   wctl --detach <PID|进程名>
 *
 * 本版本同时支持 64 位与 32 位目标: 见下面的 WCTL_SELF_64 / RelayToPeer。
 */
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <wchar.h>
#include "wctl_shared.h"

#define WCTL_EXIT_OK        0
#define WCTL_EXIT_USAGE     1
#define WCTL_EXIT_NOTFOUND  2
#define WCTL_EXIT_INJECT    3

/* ------------------------------------------------------------------ */
/* 32 / 64 位支持                                                      */
/* ------------------------------------------------------------------ */
/* CreateRemoteThread 的入口地址必须是"目标地址空间里的真实地址", 所以注入要求
 * 注入器与目标位数相同。于是同一份源码被构建成两套产物:
 *
 *     winctl.exe   (64 位) + winctl_hook.dll    -> 注入 64 位目标
 *     winctl32.exe (32 位) + winctl_hook32.dll  -> 注入 32 位目标
 *
 * 位数不匹配时, 入口程序把整条命令行原样交给同目录下另一份入口程序执行
 * (RelayToPeer), 使用者只需要记住 winctl.exe 一个入口。
 */
#ifdef _WIN64
#  define WCTL_SELF_64        1
#  define WCTL_SELF_BITS      L"64"
#  define WCTL_HOOK_DLL_NAME  L"winctl_hook.dll"
#  define WCTL_PEER_EXE       L"winctl32.exe"
#else
#  define WCTL_SELF_64        0
#  define WCTL_SELF_BITS      L"32"
#  define WCTL_HOOK_DLL_NAME  L"winctl_hook32.dll"
#  define WCTL_PEER_EXE       L"winctl.exe"
#endif

/* 子进程环境变量: 置位后不再二次转交, 避免 winctl.exe <-> winctl32.exe 来回弹 */
#define WCTL_NO_RELAY_ENV  L"WCTL_NO_RELAY"

/* 老一点的 SDK / MinGW 头文件里可能没有这几个机器类型, 补上定义 */
#ifndef IMAGE_FILE_MACHINE_ARMNT
#  define IMAGE_FILE_MACHINE_ARMNT 0x01c4
#endif
#ifndef IMAGE_FILE_MACHINE_ARM64
#  define IMAGE_FILE_MACHINE_ARM64 0xaa64
#endif

/* ------------------------------------------------------------------ */
/* 输出(兼容控制台与重定向)                                            */
/* ------------------------------------------------------------------ */
static void Out(const wchar_t* s)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
        DWORD w = 0;
        WriteConsoleW(h, s, (DWORD)wcslen(s), &w, NULL);
    } else {
        int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
        if (n > 1) {
            char* buf = (char*)malloc(n);
            if (buf) {
                DWORD w = 0;
                WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, n, NULL, NULL);
                if (h && h != INVALID_HANDLE_VALUE) WriteFile(h, buf, (DWORD)(n - 1), &w, NULL);
                free(buf);
            }
        }
    }
}

static void OutF(const wchar_t* fmt, ...)
{
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 2047, fmt, ap);
    buf[2047] = 0;
    va_end(ap);
    Out(buf);
}

static BOOL StrEq(const wchar_t* a, const wchar_t* b) { return lstrcmpiW(a, b) == 0; }
static int  IMin(int a, int b) { return a < b ? a : b; }
static BOOL StrBegins(const wchar_t* a, const wchar_t* prefix)
{
    size_t n = wcslen(prefix);
    return _wcsnicmp(a, prefix, n) == 0;
}

/* ------------------------------------------------------------------ */
/* 位数判定 / 命令行转交                                               */
/* ------------------------------------------------------------------ */
typedef BOOL (WINAPI *PFN_IsWow64Process)(HANDLE, PBOOL);
typedef BOOL (WINAPI *PFN_IsWow64Process2)(HANDLE, USHORT*, USHORT*);

/* 目标进程的位数: 32 / 64 / 0=无法判断 */
static int TargetBitness(HANDLE proc)
{
    static PFN_IsWow64Process2 p2 = NULL;
    static BOOL tried2 = FALSE;
    USHORT pm = 0, nm = 0;

    if (!tried2) {
        HMODULE k = GetModuleHandleW(L"kernel32.dll");
        if (k) p2 = (PFN_IsWow64Process2)(void*)GetProcAddress(k, "IsWow64Process2");
        tried2 = TRUE;
    }
    if (p2 && p2(proc, &pm, &nm)) {
        if (pm == IMAGE_FILE_MACHINE_I386 || pm == IMAGE_FILE_MACHINE_ARMNT) return 32;
        if (pm == IMAGE_FILE_MACHINE_UNKNOWN) {          /* 原生执行 = 与本机架构相同 */
            if (nm == IMAGE_FILE_MACHINE_I386) return 32;
            if (nm == IMAGE_FILE_MACHINE_AMD64 || nm == IMAGE_FILE_MACHINE_ARM64) return 64;
            return 0;
        }
        return 0;                                        /* ARM32 等其它 WOW 子系统 */
    }
    {
        /* 老系统(Win10 1709 之前)没有 IsWow64Process2, 退回 IsWow64Process */
        static PFN_IsWow64Process p1 = NULL;
        static BOOL tried1 = FALSE;
        SYSTEM_INFO si;
        BOOL b = FALSE;

        if (!tried1) {
            HMODULE k = GetModuleHandleW(L"kernel32.dll");
            if (k) p1 = (PFN_IsWow64Process)(void*)GetProcAddress(k, "IsWow64Process");
            tried1 = TRUE;
        }
        GetNativeSystemInfo(&si);
        if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL) return 32;  /* 纯 32 位系统 */
        if (p1 && p1(proc, &b)) return b ? 32 : 64;
    }
    return 0;
}

/* 直接从磁盘上的 exe 读 PE 头里的机器类型: 不用启动它就知道位数 */
static WORD PeMachineOfFile(const wchar_t* path)
{
    HANDLE h;
    DWORD rd = 0, sig = 0;
    IMAGE_DOS_HEADER dos;
    IMAGE_FILE_HEADER fh;
    LARGE_INTEGER li;
    WORD machine = 0;

    h = CreateFileW(path, GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (ReadFile(h, &dos, sizeof(dos), &rd, NULL) && rd == sizeof(dos) &&
        dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0) {
        li.QuadPart = dos.e_lfanew;
        if (SetFilePointerEx(h, li, NULL, FILE_BEGIN) &&
            ReadFile(h, &sig, sizeof(sig), &rd, NULL) && rd == sizeof(sig) &&
            sig == IMAGE_NT_SIGNATURE &&
            ReadFile(h, &fh, sizeof(fh), &rd, NULL) && rd == sizeof(fh))
            machine = fh.Machine;
    }
    CloseHandle(h);
    return machine;
}

/* 把"程序名 / 相对路径 / 绝对路径"解析成真实文件路径(与 CreateProcess 的搜索规则基本一致) */
static BOOL ResolveExePath(const wchar_t* target, wchar_t* out, DWORD cch)
{
    DWORD n;
    out[0] = 0;
    if (GetFileAttributesW(target) != INVALID_FILE_ATTRIBUTES) {
        lstrcpynW(out, target, (int)cch);
        return TRUE;
    }
    n = SearchPathW(NULL, target, L".exe", cch, out, NULL);
    if (n == 0 || n >= cch) { out[0] = 0; return FALSE; }
    return TRUE;
}

static BOOL EnvNoRelay(void)
{
    wchar_t buf[8];
    return GetEnvironmentVariableW(WCTL_NO_RELAY_ENV, buf, 8) > 0;
}

static void SelfDir(wchar_t* out, DWORD cch)
{
    wchar_t* slash;
    DWORD i;
    out[0] = 0;
    GetModuleFileNameW(NULL, out, cch);
    out[cch - 1] = 0;
    slash = out;
    for (i = 0; out[i]; i++) if (out[i] == L'\\' || out[i] == L'/') slash = out + i;
    *(slash + 1) = 0;
}

/* 给子进程准备环境块: 复制当前环境, 追加 WCTL_NO_RELAY=1 */
static LPVOID BuildChildEnv(void)
{
    static const wchar_t extra[] = WCTL_NO_RELAY_ENV L"=1";
    const SIZE_T extraLen = sizeof(extra) / sizeof(extra[0]);   /* 含结尾的 0 */
    LPWCH env = GetEnvironmentStringsW();
    wchar_t* p;
    wchar_t* out;
    SIZE_T len = 0;

    if (!env) return NULL;
    for (p = env; *p; p += lstrlenW(p) + 1) len += (SIZE_T)lstrlenW(p) + 1;
    out = (wchar_t*)LocalAlloc(LPTR, (len + extraLen + 1) * sizeof(wchar_t));
    if (!out) { FreeEnvironmentStringsW(env); return NULL; }
    memcpy(out, env, (len + 1) * sizeof(wchar_t));   /* 含末尾的双 0 */
    FreeEnvironmentStringsW(env);
    lstrcpyW(out + len, extra);                      /* 覆盖掉末尾那个 0, 再补回终止 */
    out[len + extraLen] = 0;
    return out;
}

/* 启动模式: 先看目标 exe 的 PE 头, 需要换一份入口程序时返回 TRUE */
static BOOL NeedPeerForFile(const wchar_t* target)
{
    wchar_t path[MAX_PATH];
    WORD m;

    if (!ResolveExePath(target, path, MAX_PATH)) return FALSE;  /* 解析不了就交回 CreateProcessW 去报错 */
    m = PeMachineOfFile(path);
    if (m == 0) return FALSE;
    if (WCTL_SELF_64)
        return !(m == IMAGE_FILE_MACHINE_AMD64 || m == IMAGE_FILE_MACHINE_ARM64);
    return (m == IMAGE_FILE_MACHINE_AMD64 || m == IMAGE_FILE_MACHINE_ARM64);
}

/* 把整条命令行原样转交给另一份入口程序(winctl.exe <-> winctl32.exe), 返回它的退出码。
   - 用 lpApplicationName 指明明细的 peer 路径, 命令行沿用原来的(本程序从 argv[1] 开始解析,
     argv[0] 是谁无所谓), 因此 --set / -- 之后的目标参数都能原样带过去。
   - 标准输入输出按原样继承, 所以控制台/重定向/GUI 抓管道的行为都不变。
   - 工作目录不传 = 子进程继承当前目录, 相对路径同样有效。 */
static int RelayToPeer(BOOL quiet)
{
    wchar_t dir[MAX_PATH], peer[MAX_PATH];
    wchar_t cmd[8192];
    LPVOID env;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE hIn, hOut, hErr;
    BOOL inherit = FALSE;
    DWORD code = 0;

    SelfDir(dir, MAX_PATH);
    lstrcpynW(peer, dir, MAX_PATH);
    lstrcatW(peer, WCTL_PEER_EXE);
    if (GetFileAttributesW(peer) == INVALID_FILE_ATTRIBUTES) {
        OutF(L"错误: 目标是 %s 位程序, 需要同目录下的 %s 才能注入, 但找不到该文件\n",
             WCTL_SELF_64 ? L"32" : L"64", WCTL_PEER_EXE);
        return WCTL_EXIT_INJECT;
    }
    if (!quiet)
        OutF(L"提示: 目标是 %s 位程序, 转交给 %s 处理\n",
             WCTL_SELF_64 ? L"32" : L"64", WCTL_PEER_EXE);

    lstrcpynW(cmd, GetCommandLineW(), 8192);
    env = BuildChildEnv();

    hIn  = GetStdHandle(STD_INPUT_HANDLE);
    hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    hErr = GetStdHandle(STD_ERROR_HANDLE);
    if (hOut && hOut != INVALID_HANDLE_VALUE) {
        SetHandleInformation(hOut, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        inherit = TRUE;
    } else hOut = NULL;
    if (hErr && hErr != INVALID_HANDLE_VALUE)
        SetHandleInformation(hErr, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    else hErr = NULL;
    if (hIn && hIn != INVALID_HANDLE_VALUE)
        SetHandleInformation(hIn, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    else hIn = NULL;

    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    if (inherit) {
        si.dwFlags    = STARTF_USESTDHANDLES;
        si.hStdInput  = hIn;
        si.hStdOutput = hOut;
        si.hStdError  = hErr;
    }
    if (!CreateProcessW(peer, cmd, NULL, NULL, inherit, CREATE_UNICODE_ENVIRONMENT,
                        env, NULL, &si, &pi)) {
        OutF(L"错误: 无法启动 %s err=%lu\n", peer, GetLastError());
        if (env) LocalFree(env);
        return WCTL_EXIT_INJECT;
    }
    if (env) LocalFree(env);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

/* ------------------------------------------------------------------ */
/* 状态位名称表                                                        */
/* ------------------------------------------------------------------ */
typedef struct FlagDef {
    const wchar_t* name;
    LONG           bit;
    LONG           cmd;
} FlagDef;

static const FlagDef kFlags[] = {
    { L"topmost",    WCTL_F_TOPMOST,    WCTL_CMD_TOPMOST    },
    { L"fullscreen", WCTL_F_FULLSCREEN, WCTL_CMD_FULLSCREEN },
    { L"lockmove",   WCTL_F_LOCKMOVE,   WCTL_CMD_LOCKMOVE   },
    { L"move",       WCTL_F_LOCKMOVE,   WCTL_CMD_LOCKMOVE   },
    { L"locksize",   WCTL_F_LOCKSIZE,   0                   },
    { L"freeze",     WCTL_F_FREEZE,     WCTL_CMD_FREEZE     },
    { L"all-windows",WCTL_F_ALLWINDOWS, 0                   },
    { L"keep-content",WCTL_F_KEEPCONTENT,0                  },
    { L"scale-content",WCTL_F_SCALECONTENT,0                },
    { L"focus",      WCTL_F_FOCUS,      WCTL_CMD_FOCUS      },
    { L"remap",      WCTL_F_REMAP,      WCTL_CMD_REMAP      },
    { L"panel",      WCTL_F_PANEL,      WCTL_CMD_PANEL      },
    { L"pin",        WCTL_F_PANEL_PIN,  WCTL_CMD_PIN        },
    { L"hotkeys",    WCTL_F_HOTKEYS,    0                   },
    { L"clickthru",  WCTL_F_CLICKTHRU,  0                   },
    { L"force-nc",   WCTL_F_FORCE_NC,   0                   },
    { L"verbose",    WCTL_F_VERBOSE,    0                   },
};
static const int kFlagNum = (int)(sizeof(kFlags) / sizeof(kFlags[0]));

static const wchar_t* kValueOpts[] = {
    L"--dll=", L"--timeout=", L"--panel-height=", L"--panel-alpha=",
    L"--panel-alpha-hot=", L"--lockpos=", L"--monitor=",
    L"--panel-pos=", L"--panel-vpos=", L"--panel-width=",
    L"--panel-x=", L"--panel-y=", L"--panel-xy=",
    L"--remap-capture=", L"--remap-scale=", L"--possim=",
};
static const int kValueOptNum = (int)(sizeof(kValueOpts) / sizeof(kValueOpts[0]));

/* ------------------------------------------------------------------ */
/* 面板"数值型"参数: 位置 / 宽度 / 透明度 / 高度 / 显示器               */
/* 命令行开关与 --set(运行时修改)共用同一套解析, 免得两处走偏。          */
/* ------------------------------------------------------------------ */
static BOOL ParsePanelPos(const wchar_t* v, LONG* out)
{
    if (StrEq(v, L"left")   || StrEq(v, L"左")   || StrEq(v, L"靠左")) { *out = WCTL_PANELPOS_LEFT;   return TRUE; }
    if (StrEq(v, L"center") || StrEq(v, L"centre") || StrEq(v, L"中") || StrEq(v, L"居中")) { *out = WCTL_PANELPOS_CENTER; return TRUE; }
    if (StrEq(v, L"right")  || StrEq(v, L"右")   || StrEq(v, L"靠右")) { *out = WCTL_PANELPOS_RIGHT;  return TRUE; }
    if (StrEq(v, L"custom") || StrEq(v, L"自定义"))                    { *out = WCTL_PANELPOS_CUSTOM; return TRUE; }
    return FALSE;
}

static BOOL ParsePanelVPos(const wchar_t* v, LONG* out)
{
    if (StrEq(v, L"top")    || StrEq(v, L"上")   || StrEq(v, L"顶部")) { *out = WCTL_PANELVPOS_TOP;    return TRUE; }
    if (StrEq(v, L"bottom") || StrEq(v, L"下")   || StrEq(v, L"底部")) { *out = WCTL_PANELVPOS_BOTTOM; return TRUE; }
    if (StrEq(v, L"custom") || StrEq(v, L"自定义"))                    { *out = WCTL_PANELVPOS_CUSTOM; return TRUE; }
    return FALSE;
}

/* "x,y" (允许负数: x<0 从右边算, y<0 从下边算) */
static BOOL ParseXY(const wchar_t* v, LONG* x, LONG* y)
{
    wchar_t* e = NULL;
    LONG a, b;
    if (!v) return FALSE;
    a = (LONG)wcstol(v, &e, 10);
    if (!e || (*e != L',' && *e != L';')) return FALSE;
    b = (LONG)wcstol(e + 1, NULL, 10);
    *x = a; *y = b;
    return TRUE;
}

/* 把 name=value 应用到共享内存; 处理不了(不是数值型参数)返回 FALSE */
static BOOL ApplyShmValue(WctlShared* s, const wchar_t* name, const wchar_t* val)
{
    LONG v;
    if (!s) return FALSE;
    if (StrEq(name, L"panel-pos")) {
        if (!ParsePanelPos(val, &v)) { OutF(L"错误: panel-pos 取值应为 left/center/right/custom\n"); return TRUE; }
        s->panelPos = v;
        return TRUE;
    }
    if (StrEq(name, L"panel-vpos")) {
        if (!ParsePanelVPos(val, &v)) { OutF(L"错误: panel-vpos 取值应为 top/bottom/custom\n"); return TRUE; }
        s->panelVPos = v;
        return TRUE;
    }
    if (StrEq(name, L"panel-width")) {
        s->panelWidth = (LONG)wcstol(val, NULL, 10);      /* 0 = 按内容自适应 */
        if (s->panelWidth < 0) s->panelWidth = 0;
        return TRUE;
    }
    if (StrEq(name, L"panel-x")) {
        s->panelX = (LONG)wcstol(val, NULL, 10);
        s->panelPos = WCTL_PANELPOS_CUSTOM;
        return TRUE;
    }
    if (StrEq(name, L"panel-y")) {
        s->panelY = (LONG)wcstol(val, NULL, 10);
        s->panelVPos = WCTL_PANELVPOS_CUSTOM;
        return TRUE;
    }
    if (StrEq(name, L"panel-xy")) {
        LONG x, y;
        if (!ParseXY(val, &x, &y)) { OutF(L"错误: panel-xy 格式应为 x,y\n"); return TRUE; }
        s->panelX = x; s->panelY = y;
        s->panelPos  = WCTL_PANELPOS_CUSTOM;
        s->panelVPos = WCTL_PANELVPOS_CUSTOM;
        return TRUE;
    }
    if (StrEq(name, L"panel-height")) {
        s->panelHeight = (LONG)wcstol(val, NULL, 10);
        return TRUE;
    }
    if (StrEq(name, L"panel-alpha")) {
        s->panelAlpha = (LONG)wcstol(val, NULL, 10);
        return TRUE;
    }
    if (StrEq(name, L"panel-alpha-hot")) {
        s->panelAlphaHot = (LONG)wcstol(val, NULL, 10);
        return TRUE;
    }
    if (StrEq(name, L"monitor")) {
        s->monitor = (LONG)wcstol(val, NULL, 10);
        return TRUE;
    }
    if (StrEq(name, L"remap-capture")) {
        if (StrEq(val, L"auto")) s->remapCapture = WCTL_REMAPCAP_AUTO;
        else if (StrEq(val, L"gdi")) s->remapCapture = WCTL_REMAPCAP_GDI;
        else if (StrEq(val, L"dwm")) s->remapCapture = WCTL_REMAPCAP_DWM;
        else { OutF(L"错误: remap-capture 取值应为 auto/gdi/dwm\n"); return TRUE; }
        return TRUE;
    }
    if (StrEq(name, L"remap-scale")) {
        if (StrEq(val, L"full")) s->remapScale = WCTL_REMAPSCALE_FULL;
        else if (StrEq(val, L"sync")) s->remapScale = WCTL_REMAPSCALE_SYNC;
        else { OutF(L"错误: remap-scale 取值应为 full/sync\n"); return TRUE; }
        return TRUE;
    }
    if (StrEq(name, L"possim")) {
        if (StrEq(val, L"on")) s->remapPosSim = WCTL_POSSIM_ON;
        else if (StrEq(val, L"off")) s->remapPosSim = WCTL_POSSIM_OFF;
        else { OutF(L"错误: possim 取值应为 on/off\n"); return TRUE; }
        return TRUE;
    }
    return FALSE;
}

static BOOL ParseOnOff(const wchar_t* v, BOOL* out)
{
    if (!v) return FALSE;
    if (StrEq(v, L"on") || StrEq(v, L"1") || StrEq(v, L"true") ||
        StrEq(v, L"yes") || StrEq(v, L"开") || StrEq(v, L"是")) { *out = TRUE;  return TRUE; }
    if (StrEq(v, L"off") || StrEq(v, L"0") || StrEq(v, L"false") ||
        StrEq(v, L"no") || StrEq(v, L"关") || StrEq(v, L"否")) { *out = FALSE; return TRUE; }
    return FALSE;
}

/* 是否是 wctl 自己的 --开关=值 */
static const FlagDef* MatchFlagOption(const wchar_t* a, const wchar_t** valOut)
{
    for (int i = 0; i < kFlagNum; i++) {
        wchar_t key[64];
        _snwprintf(key, 63, L"--%s=", kFlags[i].name);
        key[63] = 0;
        if (StrBegins(a, key)) { *valOut = a + wcslen(key); return &kFlags[i]; }
    }
    return NULL;
}

static BOOL IsValueOpt(const wchar_t* a)
{
    for (int i = 0; i < kValueOptNum; i++)
        if (StrBegins(a, kValueOpts[i])) return TRUE;
    return FALSE;
}

/* 应用一个 --flag=value 选项; 支持 --lockmove=user (锁死程序自己的移动, 但允许用户拖拽) */
static BOOL ApplyOneFlag(const FlagDef* fd, const wchar_t* val, LONG* flags)
{
    BOOL on;
    if ((fd->bit & WCTL_F_LOCKMOVE) &&
        (StrEq(val, L"user") || StrEq(val, L"drag") || StrEq(val, L"可拖") || StrEq(val, L"跟随"))) {
        *flags |= (WCTL_F_LOCKMOVE | WCTL_F_USERMOVE);
        return TRUE;
    }
    if (StrEq(val, L"toggle")) {
        if (*flags & fd->bit) {
            *flags &= ~fd->bit;
            if (fd->bit & WCTL_F_LOCKMOVE) *flags &= ~WCTL_F_USERMOVE;
        } else {
            *flags |= fd->bit;
        }
        return TRUE;
    }
    if (!ParseOnOff(val, &on)) {
        OutF(L"错误: 开关 '%s' 的取值 '%s' 无法识别 (on/off/user/toggle)\n", fd->name, val);
        return FALSE;
    }
    if (on) {
        *flags |= fd->bit;
    } else {
        *flags &= ~fd->bit;
        if (fd->bit & WCTL_F_LOCKMOVE) *flags &= ~WCTL_F_USERMOVE;
    }
    return TRUE;
}

/* name=value 形式(用于 --set), 应用到 flags / 共享内存 */
static BOOL ApplySetItem(const wchar_t* item, LONG* flags, LONG* lastCmd, WctlShared* shm)
{
    wchar_t name[64];
    const wchar_t* eq = wcschr(item, L'=');
    const wchar_t* val;
    int i;

    if (!eq) {
        OutF(L"错误: '%s' 不是 name=value 形式\n", item);
        return FALSE;
    }
    lstrcpynW(name, item, IMin(63, (int)(eq - item) + 1));
    val = eq + 1;
    for (i = 0; i < kFlagNum; i++) {
        if (StrEq(name, kFlags[i].name)) {
            if (ApplyOneFlag(&kFlags[i], val, flags)) {
                if (kFlags[i].cmd) *lastCmd = kFlags[i].cmd;
                return TRUE;
            }
            return FALSE;
        }
    }
    if (ApplyShmValue(shm, name, val)) return TRUE;
    OutF(L"错误: 未知开关 '%s'\n", name);
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* 进程 / 窗口 查找                                                    */
/* ------------------------------------------------------------------ */
typedef struct WinFind { DWORD pid; HWND best; LONG bestScore; } WinFind;

static BOOL CALLBACK FindMainProc(HWND hwnd, LPARAM lp)
{
    WinFind* fw = (WinFind*)lp;
    DWORD pid = 0;
    RECT r;
    LONG score;
    LONG_PTR ex;
    wchar_t title[4];

    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != fw->pid) return TRUE;
    if (GetParent(hwnd)) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return TRUE;
    if (!GetWindowRect(hwnd, &r)) return TRUE;
    if (r.right - r.left < 40 || r.bottom - r.top < 30) return TRUE;
    title[0] = 0;
    GetWindowTextW(hwnd, title, 3);              /* 跨进程安全, 不会发 WM_GETTEXT */
    score = (r.right - r.left) * (r.bottom - r.top) / 1000;
    if (title[0]) score += 2000000000L;          /* 优先有标题的窗口 */
    if (score > fw->bestScore) { fw->bestScore = score; fw->best = hwnd; }
    return TRUE;
}

static HWND FindMainWindow(DWORD pid)
{
    WinFind fw;
    fw.pid = pid; fw.best = NULL; fw.bestScore = -1;
    EnumWindows(FindMainProc, (LPARAM)&fw);
    return fw.best;
}

static DWORD FindProcessByName(const wchar_t* name)
{
    HANDLE snap;
    PROCESSENTRY32W pe;
    DWORD found = 0;
    wchar_t base[MAX_PATH];
    const wchar_t* p = name;

    for (const wchar_t* q = name; *q; q++) if (*q == L'\\' || *q == L'/') p = q + 1;
    lstrcpynW(base, p, MAX_PATH);
    {
        int n = lstrlenW(base);
        if (n > 4 && StrEq(base + n - 4, L".exe")) base[n - 4] = 0;
    }
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            wchar_t pbase[MAX_PATH];
            int n;
            lstrcpynW(pbase, pe.szExeFile, MAX_PATH);
            n = lstrlenW(pbase);
            if (n > 4 && StrEq(pbase + n - 4, L".exe")) pbase[n - 4] = 0;
            if (StrEq(pbase, base)) { found = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

static DWORD ResolvePid(const wchar_t* target)
{
    wchar_t* end = NULL;
    DWORD maybe = (DWORD)wcstoul(target, &end, 10);
    if (end && *end == 0 && maybe != 0) return maybe;
    return FindProcessByName(target);
}

/* ------------------------------------------------------------------ */
/* 权限 / 注入                                                         */
/* ------------------------------------------------------------------ */
static void EnableDebugPriv(void)
{
    HANDLE tok;
    TOKEN_PRIVILEGES tp;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueW(NULL, SE_DEBUG_NAME, &tp.Privileges[0].Luid))
        AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), NULL, NULL);
    CloseHandle(tok);
}

static HMODULE RemoteModuleBase(DWORD pid, const wchar_t* dllName)
{
    HANDLE snap;
    MODULEENTRY32W me;
    HMODULE base = NULL;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return NULL;
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            if (StrEq(me.szModule, dllName)) { base = (HMODULE)me.modBaseAddr; break; }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return base;
}

static BOOL InjectDll(HANDLE proc, DWORD pid, const wchar_t* dllPath, DWORD timeoutMs)
{
    SIZE_T bytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    void* remote = NULL;
    HANDLE th = NULL;
    FARPROC loadLib;
    HMODULE remoteBase = NULL, localBase;
    FARPROC localAttach;
    SIZE_T rva;
    SIZE_T wr = 0;
    const wchar_t* name = dllPath;
    int i;

    for (const wchar_t* q = dllPath; *q; q++) if (*q == L'\\' || *q == L'/') name = q + 1;

    remote = VirtualAllocEx(proc, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { OutF(L"错误: VirtualAllocEx 失败 err=%lu\n", GetLastError()); return FALSE; }
    if (!WriteProcessMemory(proc, remote, dllPath, bytes, &wr)) {
        OutF(L"错误: WriteProcessMemory 失败 err=%lu\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return FALSE;
    }
    loadLib = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    th = CreateRemoteThread(proc, NULL, 0, (LPTHREAD_START_ROUTINE)(void*)loadLib, remote, 0, NULL);
    if (!th) {
        OutF(L"错误: 无法在目标进程创建线程 err=%lu\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return FALSE;
    }
    if (WaitForSingleObject(th, timeoutMs) != WAIT_OBJECT_0) {
        OutF(L"错误: 等待 LoadLibraryW 超时\n");
        CloseHandle(th);
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return FALSE;
    }
    CloseHandle(th);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);

    /* 64 位下 GetExitCodeThread 拿不到完整 HMODULE, 用模块快照取基址 */
    for (i = 0; i < 40 && !remoteBase; i++) {
        remoteBase = RemoteModuleBase(pid, name);
        if (!remoteBase) Sleep(50);
    }
    if (!remoteBase) {
        OutF(L"错误: 目标进程中找不到已加载的 %s\n", name);
        return FALSE;
    }
    localBase = LoadLibraryW(dllPath);
    if (!localBase) { OutF(L"错误: 无法本地加载 %s\n", dllPath); return FALSE; }
    /* 32 位下 __stdcall 导出会被修饰, MinGW 实际给出的是 "WctlAttach@4"
       (cdecl 则是带前导下划线的 _WctlAttach), 这里把几种写法都试一遍,
       免得注入器挑 DLL 的编译方式。 */
    localAttach = GetProcAddress(localBase, "WctlAttach");
    if (!localAttach) localAttach = GetProcAddress(localBase, "WctlAttach@4");
    if (!localAttach) localAttach = GetProcAddress(localBase, "_WctlAttach@4");
    if (!localAttach) localAttach = GetProcAddress(localBase, "_WctlAttach");
    if (!localAttach) {
        OutF(L"错误: DLL 中找不到导出 WctlAttach\n");
        FreeLibrary(localBase);
        return FALSE;
    }
    rva = (SIZE_T)((BYTE*)localAttach - (BYTE*)localBase);
    FreeLibrary(localBase);

    th = CreateRemoteThread(proc, NULL, 0,
                            (LPTHREAD_START_ROUTINE)(void*)((BYTE*)remoteBase + rva),
                            NULL, 0, NULL);
    if (!th) {
        OutF(L"错误: 调用 WctlAttach 失败 err=%lu\n", GetLastError());
        return FALSE;
    }
    if (WaitForSingleObject(th, timeoutMs) != WAIT_OBJECT_0) {
        OutF(L"错误: WctlAttach 超时\n");
        CloseHandle(th);
        return FALSE;
    }
    {
        DWORD code = 0;
        GetExitCodeThread(th, &code);
        CloseHandle(th);
        if (code == 0) { OutF(L"错误: WctlAttach 返回失败\n"); return FALSE; }
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* 共享内存                                                            */
/* ------------------------------------------------------------------ */
typedef struct Shm {
    HANDLE      h;
    WctlShared* p;
    BOOL        own;
} Shm;

static BOOL ShmOpen(Shm* s, DWORD pid)
{
    wchar_t name[128];
    SECURITY_ATTRIBUTES sa;
    SECURITY_DESCRIPTOR sd;

    /* 目标进程可能是 SYSTEM 或其它用户, 用空 DACL 保证同会话可访问 */
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;

    s->p = NULL;
    s->h = NULL;
    s->own = FALSE;
    _snwprintf(name, 127, WCTL_SHM_FMT, pid);
    name[127] = 0;
    s->h = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0,
                              sizeof(WctlShared), name);
    if (!s->h) return FALSE;
    s->own = (GetLastError() != ERROR_ALREADY_EXISTS);
    s->p = (WctlShared*)MapViewOfFile(s->h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(WctlShared));
    if (!s->p) { CloseHandle(s->h); s->h = NULL; return FALSE; }
    if (s->own || s->p->magic != WCTL_SHM_MAGIC) {
        memset(s->p, 0, sizeof(WctlShared));
        s->p->magic          = WCTL_SHM_MAGIC;
        s->p->version        = WCTL_SHM_VERSION;
        s->p->structSize     = sizeof(WctlShared);
        s->p->flags          = WCTL_DEFAULT_FLAGS;
        s->p->monitor        = -1;
        s->p->panelHeight    = 30;
        /* 默认: 鼠标不在面板上时完全透明(alpha=0), 鼠标移上去才显示 */
        s->p->panelAlpha     = 0;
        s->p->panelAlphaHot  = 255;
        /* 默认: 面板只占"按钮需要的那点宽度", 水平居中, 贴客户区顶部 */
        s->p->panelPos       = WCTL_PANELPOS_CENTER;
        s->p->panelVPos      = WCTL_PANELVPOS_TOP;
        s->p->panelWidth     = 0;
        s->p->panelX         = 0;
        s->p->panelY         = 0;
        s->p->hotkeyTop      = 0x54;
        s->p->hotkeyFull     = 0x46;
        s->p->hotkeyLock     = 0x4C;
        s->p->hotkeyPanel    = 0x48;
        s->p->pid            = (LONG)pid;
    }
    return TRUE;
}

static void ShmClose(Shm* s)
{
    if (s->p) UnmapViewOfFile(s->p);
    if (s->h) CloseHandle(s->h);
    s->p = NULL; s->h = NULL;
}

static void PushCommand(Shm* s)
{
    if (s->p) InterlockedIncrement((LONG volatile*)&s->p->cmdSeq);
}

static void FlagText(LONG flags, wchar_t* buf)
{
    _snwprintf(buf, 511, L"置顶=%s 全屏=%s 移动锁=%s 面板=%s 穿透=%s 热键=%s 常亮=%s 内容缩放=%s 内容保持=%s 焦点模拟=%s 显示重映射=%s%s",
              (flags & WCTL_F_TOPMOST)    ? L"开" : L"关",
              (flags & WCTL_F_FULLSCREEN) ? L"开" : L"关",
              (flags & WCTL_F_LOCKMOVE)   ? L"开" : L"关",
              (flags & WCTL_F_PANEL)      ? L"开" : L"关",
              (flags & WCTL_F_CLICKTHRU)  ? L"开" : L"关",
              (flags & WCTL_F_HOTKEYS)    ? L"开" : L"关",
              (flags & WCTL_F_PANEL_PIN)  ? L"开" : L"关",
              (flags & WCTL_F_SCALECONTENT)? L"开" : L"关",
              (flags & WCTL_F_KEEPCONTENT)? L"开" : L"关",
              (flags & WCTL_F_FOCUS)      ? L"开" : L"关",
              (flags & WCTL_F_REMAP)      ? L"开" : L"关",
              (flags & WCTL_F_VERBOSE)    ? L" 日志=开" : L"");
    buf[511] = 0;
}

/* 面板位置/宽度/透明度的可读文本 */
static void PanelText(const WctlShared* p, wchar_t* buf)
{
    static const wchar_t* posName[4] = { L"靠左", L"居中", L"靠右", L"自定义" };
    static const wchar_t* vposName[3] = { L"顶部", L"底部", L"自定义" };
    const wchar_t* px = (p->panelPos  >= 0 && p->panelPos  < 4) ? posName[p->panelPos]   : L"?";
    const wchar_t* py = (p->panelVPos >= 0 && p->panelVPos < 3) ? vposName[p->panelVPos] : L"?";
    if (p->panelPos == WCTL_PANELPOS_CUSTOM || p->panelVPos == WCTL_PANELVPOS_CUSTOM)
        _snwprintf(buf, 255, L"%s/%s (%ld,%ld) 宽度=%s 高=%ld 透明度=%ld/%ld",
                   px, py, (long)p->panelX, (long)p->panelY,
                   p->panelWidth > 0 ? L"固定" : L"自适应",
                   (long)p->panelHeight, (long)p->panelAlpha, (long)p->panelAlphaHot);
    else
        _snwprintf(buf, 255, L"%s/%s 宽度=%s 高=%ld 透明度=%ld/%ld",
                   px, py, p->panelWidth > 0 ? L"固定" : L"自适应",
                   (long)p->panelHeight, (long)p->panelAlpha, (long)p->panelAlphaHot);
    if (p->panelWidth > 0) {
        wchar_t t[64];
        _snwprintf(t, 63, L"(%ld)", (long)p->panelWidth);
        t[63] = 0;
        lstrcatW(buf, t);
    }
    buf[255] = 0;
}

/* ------------------------------------------------------------------ */
/* 子命令                                                              */
/* ------------------------------------------------------------------ */
static int CmdStatus(DWORD pid, BOOL json)
{
    Shm s;
    wchar_t buf[512];

    if (!ShmOpen(&s, pid) || !s.p || s.p->magic != WCTL_SHM_MAGIC) {
        if (json) OutF(L"{\"pid\":%lu,\"injected\":false}\n", (unsigned long)pid);
        else OutF(L"进程 %lu 未被注入 (没有状态共享内存)\n", (unsigned long)pid);
        ShmClose(&s);
        return WCTL_EXIT_NOTFOUND;
    }
    if (json) {
        OutF(L"{\"pid\":%lu,\"injected\":%s,\"ready\":%ld,\"flags\":%ld,\"actual\":%ld,"
             L"\"hwnd\":%llu,\"status\":\"%s\"}\n",
             (unsigned long)pid, s.p->ready ? L"true" : L"false",
             (long)s.p->ready, (long)s.p->flags, (long)s.p->actual,
             (unsigned long long)(ULONG_PTR)WCTL_SHM_GET_HWND(s.p), (const wchar_t*)s.p->status);
        ShmClose(&s);
        return WCTL_EXIT_OK;
    }
    OutF(L"PID        : %lu\n", (unsigned long)pid);
    OutF(L"注入       : %s\n", s.p->ready ? L"是 (DLL 运行中)" : L"否 (DLL 未就绪)");
    OutF(L"目标窗口   : 0x%08X\n", (unsigned)(ULONG_PTR)WCTL_SHM_GET_HWND(s.p));
    OutF(L"目标进程   : %s\n", (const wchar_t*)s.p->targetExe);
    FlagText(s.p->flags, buf);
    OutF(L"开关(期望) : %s\n", buf);
    FlagText(s.p->actual, buf);
    OutF(L"实际状态   : %s\n", buf);
    PanelText(s.p, buf);
    OutF(L"面板       : %s\n", buf);
    OutF(L"DLL 状态   : %s\n", (const wchar_t*)s.p->status);
    ShmClose(&s);
    return WCTL_EXIT_OK;
}

static int CmdDetach(DWORD pid)
{
    Shm s;
    if (!ShmOpen(&s, pid) || !s.p || s.p->magic != WCTL_SHM_MAGIC) {
        OutF(L"错误: 进程 %lu 未被注入\n", (unsigned long)pid);
        ShmClose(&s);
        return WCTL_EXIT_NOTFOUND;
    }
    InterlockedExchange((LONG volatile*)&s.p->detach, 1);
    PushCommand(&s);
    OutF(L"已请求卸载, 等待 DLL 还原窗口...\n");
    for (int i = 0; i < 60; i++) {
        if (s.p->ready == 0) break;
        Sleep(100);
    }
    OutF(L"%s\n", s.p->ready ? L"警告: DLL 未在 6 秒内退出" : L"已卸载, 窗口已还原");
    ShmClose(&s);
    return WCTL_EXIT_OK;
}

/* ---------------- 强制关闭 ---------------- */
static BOOL CALLBACK KillProc(HWND hwnd, LPARAM lp)
{
    DWORD pid = 0;
    DWORD target = (DWORD)lp;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != target) return TRUE;
    if (GetParent(hwnd) != NULL) return TRUE;
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
    return TRUE;
}

static int CmdKill(DWORD pid)
{
    HANDLE proc;
    int i;

    proc = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_INFORMATION | SYNCHRONIZE |
                       PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                       PROCESS_VM_READ, FALSE, pid);
    if (!proc) {
        OutF(L"错误: 无法打开进程 %lu err=%lu (需要管理员权限)\n",
             (unsigned long)pid, GetLastError());
        return WCTL_EXIT_INJECT;
    }

    /* 已注入时让 DLL 在进程内给所有顶层窗口发 WM_CLOSE, 更彻底 */
    {
        Shm s;
        if (ShmOpen(&s, pid) && s.p && s.p->magic == WCTL_SHM_MAGIC && s.p->ready) {
            InterlockedExchange((LONG volatile*)&s.p->killReq, 1);
            PushCommand(&s);
            OutF(L"已通知注入的 DLL 关闭目标 (先 WM_CLOSE, 1.5 秒后强杀)\n");
        } else {
            EnumWindows(KillProc, (LPARAM)pid);
            OutF(L"已向目标所有顶层窗口发送 WM_CLOSE, 等待退出...\n");
        }
        ShmClose(&s);
    }

    for (i = 0; i < 30; i++) {
        if (WaitForSingleObject(proc, 100) == WAIT_OBJECT_0) {
            OutF(L"目标已退出\n");
            CloseHandle(proc);
            return WCTL_EXIT_OK;
        }
    }
    OutF(L"目标不接受 WM_CLOSE, 强制结束进程\n");
    if (!TerminateProcess(proc, 0)) {
        OutF(L"强制结束失败 err=%lu\n", GetLastError());
        CloseHandle(proc);
        return WCTL_EXIT_INJECT;
    }
    WaitForSingleObject(proc, 2000);
    OutF(L"已强制结束\n");
    CloseHandle(proc);
    return WCTL_EXIT_OK;
}

/* ------------------------------------------------------------------ */
static void Usage(void)
{
    Out(L"wctl -- 窗口控制注入器 (置顶 / 全屏 / 移动锁 + 目标窗口顶部控制面板)\n"
        L"\n"
        L"用法:\n"
        L"  wctl [选项] <程序路径> [-- 程序参数...]   启动目标程序并注入\n"
        L"  wctl --attach <PID|进程名> [选项]          注入到已在运行的进程\n"
        L"  wctl --status <PID|进程名> [--json]        查看当前状态\n"
        L"  wctl --set <PID|进程名> 开关=值 [...]      运行时修改开关\n"
        L"  wctl --detach <PID|进程名>                 卸载并还原目标窗口\n"
        L"\n"
        L"核心开关(前三个默认关, 其余默认开):\n"
        L"  --topmost=on|off      开=强制置顶; 关=吞掉目标程序所有置顶请求\n"
        L"  --fullscreen=on|off   开=强制全屏; 关=退出全屏并强制显示(没有就创建)标题栏\n"
        L"  --lockmove=on|off|user 开=吞掉目标所有移动请求; user=只吞程序自己发的, 允许你手动拖拽\n"
        L"  --panel=on|off        显示控制面板          --pin=on|off 面板常亮不淡出\n"
        L"  --locksize=on|off     锁定窗口大小(吞掉所有改变尺寸的请求)\n"
        L"  --freeze=on|off       锁定窗口状态: 勾选那一刻的置顶/位置/大小/全屏全部固化,\n"
        L"                        并且**禁止最小化/最大化/全屏/移动/缩放**(窗口被彻底钉死,\n"
        L"                        外部进程强改也按回去); 重建窗口后继续生效\n"
        L"  --all-windows         全屏/位置/大小 规则应用到进程内所有顶层窗口(多窗口程序)\n"
        L"  --scale-content=on|off 内容不随窗口缩放的目标(远程桌面/屏幕广播/教室软件学生端)\n"
        L"                        窗口化时把远端画面**等比缩放**到窗口里显示(画面不裁掉,\n"
        L"                        窗口也不会比屏幕大); 靠重定向目标程序的内容视图子窗口绘制\n"
        L"                        实现, 只对用 GDI 绘制的程序有效(默认开)\n"
        L"  --keep-content=on|off 上面那个关掉时的退路: 内容不随窗口缩放的目标窗口化时\n"
        L"                        保持原始内容尺寸, 宁可比屏幕大也不裁掉内容(默认开)\n"
        L"  --focus=on|off        焦点模拟: 目标询问\"我是不是前台焦点\"时回答\"是\",\n"
        L"                        并把 WM_ACTIVATEAPP/WM_ACTIVATE/WM_NCACTIVATE 改写成激活,\n"
        L"                        吞掉 WM_KILLFOCUS。适合\"失去焦点就暂停/停止刷新\"的程序\n"
        L"  --remap=on|off        显示重映射(第二种实现): 把源窗口按原始内容尺寸移到所有\n"
        L"                        显示器之外, 再在桌面开一个查看器显示抓取到的画面(等比缩放,\n"
        L"                        多余部分黑色填充); 画面区域内的点击/滚轮/键盘会反向换算后\n"
        L"                        投递给源窗口。关闭查看器窗口即停止并还原(默认关)\n"
        L"  --remap-capture=<方式> 重映射抓取方式: auto(默认, 先 PrintWindow/BitBlt,\n"
        L"                        失败再退到 DWM) | gdi | dwm\n"
        L"  --remap-scale=<规则>  重映射缩放规则: full(默认, 源窗口拉成屏幕大小再映射) |\n"
        L"                        sync(源窗口保持原尺寸, 映射窗口 = 原窗口大小)\n"
        L"  --possim=on|off       窗口位置模拟(默认开): 目标询问窗口位置/大小时,\n"
        L"                        回答\"用户实际看到的样子\"(sync=查看器, full=全屏)\n"
        L"  --hotkeys=on|off      注册全局热键 Ctrl+Alt+T/F/L/H\n"
        L"  --clickthru=on|off    面板空闲时鼠标穿透, 不影响目标操作(默认开)\n"
        L"  --force-nc=on|off     用系统方式强制绘制标题栏(默认开)\n"
        L"  --verbose             写日志到 %%TEMP%%\\wctl-<pid>.log\n"
        L"\n"
        L"针对\"无边框全屏霸屏 + 疯狂抢置顶 + 关不掉\"的软件:\n"
        L"  --windowed              一键窗口化: 取消全屏 + 补标题栏 + 按自动尺寸重摆\n"
        L"                          (工作区 70%% 居中, 可拖拽可缩放) + lockmove=user\n"
        L"  --autosize              只重摆一次窗口尺寸\n"
        L"  --kill                  关闭目标: 先 WM_CLOSE, 1.5 秒不接受就强制结束进程\n"
        L"\n"
        L"外观 / 其它:\n"
        L"  --panel-height=<n>      面板逻辑高度(96dpi 像素, 默认 30)\n"
        L"  --panel-width=<n>       面板逻辑宽度; 0(默认)=按按钮内容自适应, 只占需要的那点\n"
        L"  --panel-pos=<位置>      水平位置: left | center(默认) | right | custom\n"
        L"  --panel-vpos=<位置>     垂直位置: top(默认) | bottom | custom\n"
        L"  --panel-x=<n>           panel-pos=custom 时的水平坐标(负数=从右边算起)\n"
        L"  --panel-y=<n>           panel-vpos=custom 时的垂直坐标(负数=从下边算起)\n"
        L"  --panel-xy=<x,y>        一次指定自定义坐标(等价于 --panel-x 加 --panel-y)\n"
        L"  --panel-alpha=<0-255>   面板空闲不透明度(默认 0: 鼠标不在上面时完全透明)\n"
        L"  --panel-alpha-hot=<n>   鼠标在面板上时的不透明度(默认 255)\n"
        L"  --lockpos=<x,y>         指定移动锁坐标(默认锁定当前坐标)\n"
        L"  --monitor=<n>           指定显示器序号(0 起, 默认跟随窗口)\n"
        L"  --dll=<路径>            指定注入 DLL(默认按目标位数自动选:\n"
        L"                          64 位目标=同目录 winctl_hook.dll, 32 位=winctl_hook32.dll)\n"
        L"  --timeout=<ms>          注入等待超时(默认 15000)\n"
        L"  --wait                  等待目标进程退出后再返回\n"
        L"  --no-inject             只启动不注入\n"
        L"  --json                  状态以 JSON 输出\n"
        L"  -h, --help              显示本帮助\n"
        L"\n"
        L"--set 也能改上面这些数值型参数, 例如:\n"
        L"  wctl --set Client.exe panel-pos=right panel-width=0\n"
        L"  wctl --set Client.exe panel-xy=40,60 panel-alpha=110\n"
        L"\n"
        L"32 位目标:\n"
        L"  目标是 32 位程序时, 本程序会自动把命令转交给同目录下的 winctl32.exe\n"
        L"  (32 位入口 + winctl_hook32.dll) 执行, 输出与退出码原样返回, 不需要手动切换。\n"
        L"\n"
        L"示例:\n"
        L"  wctl --windowed --kill Client.exe\n"
        L"  wctl --freeze --all-windows Client.exe\n"
        L"  wctl --topmost=off --fullscreen=off --lockmove=on \"C:\\App\\client.exe\"\n"
        L"  wctl --status client.exe\n"
        L"  wctl --set client.exe fullscreen=off\n"
        L"  wctl --detach client.exe\n");
}

/* ------------------------------------------------------------------ */
int main(void)
{
    int argc = 0;
    wchar_t** argv;
    wchar_t dllPath[MAX_PATH];
    wchar_t target[1024];
    wchar_t extra[2048];
    wchar_t* setItems[64];
    int  setCount = 0;
    LONG flags = 0, lastCmd = 0;
    BOOL anyFlagOpt = FALSE, lockmoveGiven = FALSE;
    int  mode = 0;                     /* 0=启动 1=attach 2=status 3=detach 4=set 5=kill */
    BOOL json = FALSE, doWait = FALSE, noInject = FALSE;
    BOOL optVerbose = FALSE, optNoClickthru = FALSE;
    BOOL windowedPreset = FALSE, wantAutoSize = FALSE;
    BOOL optAllWin = FALSE, optFreeze = FALSE;
    DWORD timeout = 15000;
    LONG oPanelH = -1, oPanelA = -1, oPanelAH = -1, oMonitor = -2, oRemapCapture = -1;
    LONG oPanelW = -1, oPanelPos = -1, oPanelVPos = -1, oPanelX = 0, oPanelY = 0;
    BOOL havePanelX = FALSE, havePanelY = FALSE;
    BOOL haveLockPos = FALSE;
    LONG lockX = 0, lockY = 0;
    DWORD pid = 0;
    HWND hwnd = NULL;
    Shm shm;
    int i, rc = WCTL_EXIT_OK;
    BOOL seenDashDash = FALSE;
    BOOL targetLocked = FALSE;

    SetConsoleOutputCP(CP_UTF8);
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return WCTL_EXIT_USAGE;

    SelfDir(dllPath, MAX_PATH);
    lstrcatW(dllPath, WCTL_HOOK_DLL_NAME);
    target[0] = 0;
    extra[0] = 0;

    /* ---------------- 参数解析 ---------------- */
    for (i = 1; i < argc; i++) {
        wchar_t* a = argv[i];
        const wchar_t* val = NULL;
        const FlagDef* fd;

        if (!seenDashDash && a[0] == L'-' && a[1] == L'-' && a[2] == 0) {
            seenDashDash = TRUE;                      /* 后面全部给目标程序 */
            continue;
        }
        if (!seenDashDash && !StrEq(a, L"-")) {
            if (StrEq(a, L"-h") || StrEq(a, L"--help")) { Usage(); LocalFree(argv); return WCTL_EXIT_OK; }
            if (StrEq(a, L"--attach")) { mode = 1; continue; }
            if (StrEq(a, L"--status")) { mode = 2; continue; }
            if (StrEq(a, L"--detach")) { mode = 3; continue; }
            if (StrEq(a, L"--set"))    { mode = 4; continue; }
            if (StrEq(a, L"--kill"))   { mode = 5; continue; }
            if (StrEq(a, L"--windowed")) { windowedPreset = TRUE; continue; }
            if (StrEq(a, L"--autosize")) { wantAutoSize = TRUE; continue; }
            if (StrEq(a, L"--all-windows")) { optAllWin = TRUE; continue; }
            if (StrEq(a, L"--freeze")) { optFreeze = TRUE; continue; }
            if (StrEq(a, L"--json"))   { json = TRUE; continue; }
            if (StrEq(a, L"--wait"))   { doWait = TRUE; continue; }
            if (StrEq(a, L"--no-inject")) { noInject = TRUE; continue; }
            if (StrEq(a, L"--verbose")) { optVerbose = TRUE; continue; }
            if (StrEq(a, L"--no-clickthru")) { optNoClickthru = TRUE; continue; }
            if (StrBegins(a, L"--dll=")) { lstrcpynW(dllPath, a + 6, MAX_PATH); continue; }
            if (StrBegins(a, L"--timeout=")) { timeout = (DWORD)wcstoul(a + 10, NULL, 10); continue; }
            if (StrBegins(a, L"--panel-height=")) { oPanelH = (LONG)wcstoul(a + 15, NULL, 10); continue; }
            if (StrBegins(a, L"--panel-alpha-hot=")) { oPanelAH = (LONG)wcstoul(a + 18, NULL, 10); continue; }
            if (StrBegins(a, L"--panel-alpha=")) { oPanelA = (LONG)wcstoul(a + 14, NULL, 10); continue; }
            if (StrBegins(a, L"--panel-pos=")) {
                LONG v;
                if (!ParsePanelPos(a + 12, &v)) {
                    OutF(L"错误: --panel-pos 取值应为 left/center/right/custom\n");
                    LocalFree(argv); return WCTL_EXIT_USAGE;
                }
                oPanelPos = v;
                continue;
            }
            if (StrBegins(a, L"--panel-vpos=")) {
                LONG v;
                if (!ParsePanelVPos(a + 13, &v)) {
                    OutF(L"错误: --panel-vpos 取值应为 top/bottom/custom\n");
                    LocalFree(argv); return WCTL_EXIT_USAGE;
                }
                oPanelVPos = v;
                continue;
            }
            if (StrBegins(a, L"--panel-width=")) { oPanelW = (LONG)wcstoul(a + 14, NULL, 10); continue; }
            if (StrBegins(a, L"--panel-xy=")) {
                if (!ParseXY(a + 11, &oPanelX, &oPanelY)) {
                    OutF(L"错误: --panel-xy 格式应为 x,y\n");
                    LocalFree(argv); return WCTL_EXIT_USAGE;
                }
                oPanelPos = WCTL_PANELPOS_CUSTOM;
                oPanelVPos = WCTL_PANELVPOS_CUSTOM;
                havePanelX = havePanelY = TRUE;
                continue;
            }
            if (StrBegins(a, L"--panel-x=")) {
                oPanelX = (LONG)wcstol(a + 10, NULL, 10);
                oPanelPos = WCTL_PANELPOS_CUSTOM;
                havePanelX = TRUE;
                continue;
            }
            if (StrBegins(a, L"--panel-y=")) {
                oPanelY = (LONG)wcstol(a + 10, NULL, 10);
                oPanelVPos = WCTL_PANELVPOS_CUSTOM;
                havePanelY = TRUE;
                continue;
            }
            if (StrBegins(a, L"--monitor=")) { oMonitor = (LONG)wcstoul(a + 10, NULL, 10); continue; }
            if (StrBegins(a, L"--remap-capture=")) {
                const wchar_t* v = a + 16;
                if (StrEq(v, L"auto")) oRemapCapture = WCTL_REMAPCAP_AUTO;
                else if (StrEq(v, L"gdi")) oRemapCapture = WCTL_REMAPCAP_GDI;
                else if (StrEq(v, L"dwm")) oRemapCapture = WCTL_REMAPCAP_DWM;
                else {
                    OutF(L"错误: --remap-capture 取值应为 auto/gdi/dwm\n");
                    LocalFree(argv); return WCTL_EXIT_USAGE;
                }
                continue;
            }
            if (StrBegins(a, L"--lockpos=")) {
                const wchar_t* v = a + 10;
                wchar_t* e2 = NULL;
                lockX = (LONG)wcstol(v, &e2, 10);
                if (e2 && (*e2 == L',' || *e2 == L';')) {
                    lockY = (LONG)wcstol(e2 + 1, NULL, 10);
                    haveLockPos = TRUE;
                } else OutF(L"警告: --lockpos 格式应为 x,y\n");
                continue;
            }
            fd = MatchFlagOption(a, &val);
            if (fd) {
                if (!ApplyOneFlag(fd, val, &flags)) { LocalFree(argv); return WCTL_EXIT_USAGE; }
                anyFlagOpt = TRUE;
                if (fd->bit & WCTL_F_LOCKMOVE) lockmoveGiven = TRUE;
                continue;
            }
            if (IsValueOpt(a)) { OutF(L"警告: 未知选项 %s\n", a); continue; }
            if (a[0] == L'-' && a[1] == L'-') { OutF(L"警告: 忽略未知选项 %s\n", a); continue; }
        }

        if (mode == 4) {                              /* --set: 目标名之后是开关 */
            if (wcschr(a, L'=') || !target[0]) {
                if (!target[0]) lstrcpynW(target, a, 1024);
                else if (setCount < 64) setItems[setCount++] = a;
                continue;
            }
        }
        if ((mode == 1 || mode == 2 || mode == 3 || mode == 5) && !target[0]) {
            lstrcpynW(target, a, 1024);
            continue;
        }
        if (!target[0]) {
            lstrcpynW(target, a, 1024);
            targetLocked = TRUE;
            continue;
        }
        /* 目标程序自己的参数 */
        if (extra[0]) lstrcatW(extra, L" ");
        {
            wchar_t q[1200];
            if (wcschr(a, L' ')) { _snwprintf(q, 1199, L"\"%s\"", a); q[1199] = 0; }
            else lstrcpynW(q, a, 1200);
            lstrcatW(extra, q);
        }
    }
    (void)targetLocked;

    if (!target[0]) { Usage(); LocalFree(argv); return WCTL_EXIT_USAGE; }

    EnableDebugPriv();

    /* ---------------- 模式: status / detach / kill ---------------- */
    if (mode == 2 || mode == 3 || mode == 5) {
        pid = ResolvePid(target);
        if (!pid) { OutF(L"错误: 找不到进程 '%s'\n", target); LocalFree(argv); return WCTL_EXIT_NOTFOUND; }
        if (mode == 3)      rc = CmdDetach(pid);
        else if (mode == 5) rc = CmdKill(pid);
        else                rc = CmdStatus(pid, json);
        LocalFree(argv);
        return rc;
    }

    /* ---------------- 位数不匹配时转交给另一份入口程序 ----------------
       只有"真正要注入"的路径需要注意位数; status / detach / kill 只是读写共享内存
       和发窗口消息, 位数无关, 上面已经处理完了。 */
    if (!noInject && !EnvNoRelay()) {
        if (mode == 0) {
            /* 启动模式: 先读目标 exe 的 PE 头, 决定由谁去启动它 */
            if (NeedPeerForFile(target)) {
                LocalFree(argv);
                return RelayToPeer(json);
            }
        } else {
            /* --attach / --set: 目标已经在跑, 直接问系统它是几位 */
            DWORD rpid = ResolvePid(target);
            if (!rpid) { OutF(L"错误: 找不到进程 '%s'\n", target); LocalFree(argv); return WCTL_EXIT_NOTFOUND; }
            {
                HANDLE q = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, rpid);
                int tb = q ? TargetBitness(q) : 0;
                if (q) CloseHandle(q);
                if (tb != 0 && tb != (WCTL_SELF_64 ? 64 : 32)) {
                    LocalFree(argv);
                    return RelayToPeer(json);
                }
            }
        }
    }

    /* ---------------- 启动目标程序 ---------------- */
    {
        PROCESS_INFORMATION pi;
        STARTUPINFOW si;
        HANDLE proc = NULL;
        BOOL started = FALSE;

        memset(&pi, 0, sizeof(pi));
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);

        if (mode == 0) {
            wchar_t cmd[4096];
            wchar_t exeQ[1200];
            lstrcpynW(exeQ, target, 1024);
            if (exeQ[0] != L'"' && wcschr(exeQ, L' ')) {
                wchar_t t[1200];
                _snwprintf(t, 1199, L"\"%s\"", exeQ);
                t[1199] = 0;
                lstrcpynW(exeQ, t, 1200);
            }
            lstrcpynW(cmd, exeQ, 1200);
            if (extra[0]) { lstrcatW(cmd, L" "); lstrcatW(cmd, extra); }
            if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                                CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP |
                                CREATE_UNICODE_ENVIRONMENT, NULL, NULL, &si, &pi)) {
                OutF(L"错误: 无法启动 \"%s\" err=%lu\n", target, GetLastError());
                LocalFree(argv);
                return WCTL_EXIT_NOTFOUND;
            }
            pid = pi.dwProcessId;
            proc = pi.hProcess;
            started = TRUE;
            OutF(L"已启动: %s (PID %lu)\n", target, (unsigned long)pid);
        } else {
            pid = ResolvePid(target);
            if (!pid) { OutF(L"错误: 找不到进程 '%s'\n", target); LocalFree(argv); return WCTL_EXIT_NOTFOUND; }
            proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                               PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                               FALSE, pid);
            if (!proc) {
                OutF(L"错误: 无法打开进程 %lu err=%lu (需要管理员权限)\n",
                     (unsigned long)pid, GetLastError());
                LocalFree(argv);
                return WCTL_EXIT_INJECT;
            }
        }

        /* ---------------- 共享内存(按最终 pid 建立) ---------------- */
        if (!ShmOpen(&shm, pid)) {
            OutF(L"错误: 无法创建状态共享内存 err=%lu\n", GetLastError());
            if (started) TerminateProcess(proc, 1);
            CloseHandle(proc);
            LocalFree(argv);
            return WCTL_EXIT_INJECT;
        }

        /* 叠加命令行开关 */
        {
            LONG base = shm.p->flags;
            if (anyFlagOpt) {
                for (i = 0; i < argc; i++) {
                    const wchar_t* v2 = NULL;
                    const FlagDef* fd2 = MatchFlagOption(argv[i], &v2);
                    if (fd2) {
                        if (ApplyOneFlag(fd2, v2, &base) && fd2->cmd) lastCmd = fd2->cmd;
                    }
                }
            }
            if (optVerbose) base |= WCTL_F_VERBOSE;
            if (optNoClickthru) base &= ~WCTL_F_CLICKTHRU;
            if (optAllWin) base |= WCTL_F_ALLWINDOWS;
            if (optFreeze) base |= WCTL_F_FREEZE;

            /* --set 项 */
            if (mode == 4) {
                for (i = 0; i < setCount; i++)
                    if (!ApplySetItem(setItems[i], &base, &lastCmd, shm.p)) {
                        if (started) TerminateProcess(proc, 1);
                        ShmClose(&shm); CloseHandle(proc); LocalFree(argv);
                        return WCTL_EXIT_USAGE;
                    }
            } else if (mode == 0) {
                /* 启动模式默认: 三项功能都按"关"处理(见 README) */
                base &= ~(WCTL_F_TOPMOST | WCTL_F_FULLSCREEN | WCTL_F_LOCKMOVE | WCTL_F_USERMOVE);
                for (i = 0; i < argc; i++) {
                    const wchar_t* v2 = NULL;
                    const FlagDef* fd2 = MatchFlagOption(argv[i], &v2);
                    if (fd2) {
                        if (ApplyOneFlag(fd2, v2, &base) && fd2->cmd) lastCmd = fd2->cmd;
                    }
                }
            }
            /* --windowed: 一键变回"普通可拖拽窗口": 取消全屏 + 保证标题栏 +
               锁住程序自己的移动请求但允许你手动拖动 + 按自动尺寸重摆 */
            if (windowedPreset) {
                base &= ~WCTL_F_FULLSCREEN;
                base |= WCTL_F_PANEL;
                if (!lockmoveGiven) base |= (WCTL_F_LOCKMOVE | WCTL_F_USERMOVE);
            }
            shm.p->flags = base;
            if (oPanelH >= 0)  shm.p->panelHeight = oPanelH;
            if (oPanelA >= 0)  shm.p->panelAlpha = oPanelA;
            if (oPanelAH >= 0) shm.p->panelAlphaHot = oPanelAH;
            if (oPanelW >= 0)  shm.p->panelWidth = oPanelW;
            if (oPanelPos >= 0)  shm.p->panelPos = oPanelPos;
            if (oPanelVPos >= 0) shm.p->panelVPos = oPanelVPos;
            if (havePanelX) shm.p->panelX = oPanelX;
            if (havePanelY) shm.p->panelY = oPanelY;
            if (oMonitor != -2) shm.p->monitor = oMonitor;
            if (oRemapCapture >= 0) shm.p->remapCapture = oRemapCapture;
            if (haveLockPos) {
                shm.p->lockX = lockX; shm.p->lockY = lockY; shm.p->lockValid = 1;
            }
            if (lastCmd) shm.p->lastCmd = lastCmd;
            if (!(base & WCTL_F_LOCKMOVE)) shm.p->lockValid = 0;
        }
        PushCommand(&shm);

        /* ---------------- 注入 ---------------- */
        if (!noInject) {
            /* 正常情况下走到这里时位数一定匹配(不匹配的已经 RelayToPeer 转交出去了);
               这里只是兜底: 用户手工设置 WCTL_NO_RELAY, 或者目标是 ARM64 之类。 */
            int tb = TargetBitness(proc);
            if (tb != 0 && tb != (WCTL_SELF_64 ? 64 : 32)) {
                OutF(L"错误: 目标是 %d 位进程, 本程序是 %s 位, 无法直接注入。\n"
                     L"      请确认同目录下存在 %s (build.cmd 会自动构建它)。\n",
                     tb, WCTL_SELF_BITS, WCTL_PEER_EXE);
                if (started) TerminateProcess(proc, 1);
                ShmClose(&shm); CloseHandle(proc); LocalFree(argv);
                return WCTL_EXIT_INJECT;
            }
            if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES) {
                OutF(L"错误: 找不到注入 DLL: %s\n", dllPath);
                if (started) TerminateProcess(proc, 1);
                ShmClose(&shm); CloseHandle(proc); LocalFree(argv);
                return WCTL_EXIT_INJECT;
            }
            OutF(L"注入 %s ...\n", dllPath);
            if (!InjectDll(proc, pid, dllPath, timeout)) {
                if (started) TerminateProcess(proc, 1);
                ShmClose(&shm); CloseHandle(proc); LocalFree(argv);
                return WCTL_EXIT_INJECT;
            }
            {
                DWORD waited = 0;
                while (waited < timeout && shm.p->ready == 0) { Sleep(50); waited += 50; }
                if (shm.p->ready == 0) OutF(L"警告: DLL 未在 %lu ms 内就绪\n", (unsigned long)timeout);
            }
        }

        if (started) {
            ResumeThread(pi.hThread);
            CloseHandle(pi.hThread);
        }

        /* ---------------- 找到主窗口并告知 DLL ---------------- */
        {
            DWORD waited = 0;
            HWND best = NULL;
            while (waited < timeout) {
                best = FindMainWindow(pid);
                if (best) break;
                Sleep(50);
                waited += 50;
            }
            hwnd = best;
            if (best) {
                WCTL_SHM_SET_HWND(shm.p, best);
                OutF(L"目标窗口: 0x%08X\n", (unsigned)(ULONG_PTR)best);
            } else {
                OutF(L"提示: 暂未发现目标主窗口, DLL 会自动接管后出现的窗口\n");
            }
        }
        PushCommand(&shm);

        /* 窗口化自动尺寸: 等主窗口解析出来之后再触发 */
        if ((windowedPreset || wantAutoSize) && hwnd) {
            InterlockedExchange((LONG volatile*)&shm.p->resizeReq, 1);
            PushCommand(&shm);
        }

        if (json) {
            OutF(L"{\"pid\":%lu,\"hwnd\":%llu,\"injected\":%s,\"flags\":%ld}\n",
                 (unsigned long)pid, (unsigned long long)(ULONG_PTR)hwnd,
                 noInject ? L"false" : L"true", (long)shm.p->flags);
        } else {
            wchar_t buf[512];
            FlagText(shm.p->flags, buf);
            OutF(L"完成 (PID %lu)\n当前开关: %s\n", (unsigned long)pid, buf);
            if (shm.p->flags & WCTL_F_PANEL) {
                if (shm.p->panelAlpha > 0)
                    OutF(L"控制面板贴在目标窗口客户区(不透明度 %ld), 鼠标移上去变 %ld; "
                         L"按需要的那点宽度%s放\n",
                         (long)shm.p->panelAlpha, (long)shm.p->panelAlphaHot,
                         (shm.p->panelPos == WCTL_PANELPOS_LEFT)  ? L"靠左" :
                         (shm.p->panelPos == WCTL_PANELPOS_RIGHT) ? L"靠右" :
                         (shm.p->panelPos == WCTL_PANELPOS_CUSTOM)? L"按自定义坐标" : L"居中");
                else
                    OutF(L"控制面板平时完全透明(看不见), 鼠标移到那一小块上才显示; 位置/宽度可用 "
                         L"--panel-pos / --panel-width 调, 常显用 --panel-alpha=110\n");
                OutF(L"热键 Ctrl+Alt+T 置顶 / F 全屏 / L 移动锁 / H 显隐面板\n");
            }
        }

        if (doWait) {
            OutF(L"等待目标进程退出...\n");
            WaitForSingleObject(proc, INFINITE);
            OutF(L"目标进程已退出\n");
        }
        CloseHandle(proc);
        ShmClose(&shm);
    }

    LocalFree(argv);
    return rc;
}
