#include "pch.h"
#include "TVCShared.h"

static UINT g_uMsgInit = 0;
static UINT g_uMsgAlive = 0;
static UINT g_uMsgShutdown = 0;
static UINT g_uMsgRefresh = 0;
static bool g_bMessagesReady = false;

static HWND g_hHostWnd = NULL;

static void EnsureMessages()
{
    if (g_bMessagesReady) return;

    g_uMsgInit = RegisterWindowMessageW(TVC_MSG_INIT);
    g_uMsgAlive = RegisterWindowMessageW(TVC_MSG_ALIVE);
    g_uMsgShutdown = RegisterWindowMessageW(TVC_MSG_SHUTDOWN);
    g_uMsgRefresh = RegisterWindowMessageW(TVC_MSG_REFRESH);

    g_bMessagesReady = true;
}

// ---------------------------------------------------------------------------- tooltip

// Re-shows the tray tooltip for the volume icon. This has to run inside explorer.exe (only
// the process owning the notification area can pop its tooltip) and it has to run after the
// level was changed, because the shell is what refreshes the tooltip text - that is exactly
// what makes the text above the icon follow the wheel.
//
// Everything here is timeout guarded: the toolbar and its tooltip live on the taskbar
// thread and the shell's UI thread must never block waiting for it.
static void RefreshTrayTooltip()
{
    HWND hWndTray = FindTrayToolbarWindow();
    if (!hWndTray) return;

    DWORD_PTR dwResult = 0;
    if (!SendMessageTimeoutW(hWndTray, TB_GETTOOLTIPS, 0, 0, SMTO_ABORTIFHUNG, 100, &dwResult)) return;

    HWND hWndTooltip = (HWND)dwResult;
    if (!hWndTooltip) return;

    // Twice: the first pop updates a tooltip that is already on screen, the second one shows
    // it again with the text the shell has just refreshed.
    SendMessageTimeoutW(hWndTooltip, TTM_POPUP, 0, 0, SMTO_ABORTIFHUNG, 100, &dwResult);
    SendMessageTimeoutW(hWndTooltip, TTM_POPUP, 0, 0, SMTO_ABORTIFHUNG, 100, &dwResult);
}

// ---------------------------------------------------------------------------- hooks

extern "C" __declspec(dllexport) LRESULT CALLBACK CallWndProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    EnsureMessages();

    if (nCode >= HC_ACTION && g_uMsgInit)
    {
        LPCWPSTRUCT cwps = (LPCWPSTRUCT)lParam;

        if (cwps->message == g_uMsgInit)
        {
            g_hHostWnd = (HWND)cwps->lParam;
            if (g_hHostWnd) PostMessageW(g_hHostWnd, g_uMsgAlive, 0, 0);
        }
        else if (cwps->message == g_uMsgShutdown)
        {
            g_hHostWnd = NULL;
        }
        else if (cwps->message == g_uMsgRefresh)
        {
            RefreshTrayTooltip();
        }
    }

    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

BOOL APIENTRY DllMain(
    HMODULE hModule,
    DWORD  ul_reason_for_call,
    LPVOID lpReserved
)
{
    return TRUE;
}
