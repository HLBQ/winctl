/* 
    dllmain.cpp -- 注入入口
 */
#include "wctl_core.h"

static volatile LONG s_started = 0;

static DWORD WINAPI InitThread(LPVOID param)
{
    (void)param;
    WctlRun();
    return 0;
}

extern "C" __declspec(dllexport) DWORD WINAPI WctlVersion(LPVOID param)
{
    (void)param;
    return (DWORD)WCTL_SHM_VERSION;
}

extern "C" __declspec(dllexport) DWORD WINAPI WctlAttach(LPVOID param)
{
    (void)param;

    if (InterlockedCompareExchange((LONG volatile*)&s_started, 1, 0) != 0) {
        if (!g_detached) return 1;      /* 已经在运行 */

        /* 之前卸载过: 重新启动 */
        g_detached = 0;
        g_stop = 0;
        g_primary = NULL;
        g_panel = NULL;
        if (g_shm) { g_shm->detach = 0; g_shm->ready = 0; }
        if (CreateThread(NULL, 0, InitThread, NULL, 0, NULL)) return 1;
        return 0;
    }
    if (CreateThread(NULL, 0, InitThread, NULL, 0, NULL)) return 1;
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_hinst = hinst;
        DisableThreadLibraryCalls(hinst);
        InitializeCriticalSection(&g_cs);
    }
    return TRUE;
}
