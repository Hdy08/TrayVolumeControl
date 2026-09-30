#include "pch.h"
#include "TVCShared.h"

// This DLL is injected into explorer.exe and runs on the tray icon's UI thread, so it only
// observes input and forwards it. Everything slow - the audio COM calls above all - is done
// by the host process, in its own process.
//
// Blocking the shell's UI thread here makes the shell look hung, and Windows answers that by
// restarting explorer.exe (which also leaves the volume UI claiming that no speakers or
// headphones are plugged in). For the same reason this DLL never subclasses a shell window
// and never does work while it is being unloaded: there is no state that has to be cleaned
// up from DllMain.

static UINT g_uMsgInit = 0;
static UINT g_uMsgAlive = 0;
static UINT g_uMsgWheel = 0;
static UINT g_uMsgMute = 0;
static UINT g_uMsgShutdown = 0;
static UINT g_uMsgRefresh = 0;
static bool g_bMessagesReady = false;

static HWND g_hIconWnd = NULL;      // volume icon owner window = raw input target
static HWND g_hHostWnd = NULL;      // host process window receiving our requests
static RECT g_rcIconCached = { 0 };
static DWORD g_dwIconCachedTick = 0;

static void EnsureMessages()
{
    if (g_bMessagesReady) return;

    g_uMsgInit = RegisterWindowMessageW(TVC_MSG_INIT);
    g_uMsgAlive = RegisterWindowMessageW(TVC_MSG_ALIVE);
    g_uMsgWheel = RegisterWindowMessageW(TVC_MSG_WHEEL);
    g_uMsgMute = RegisterWindowMessageW(TVC_MSG_MUTE);
    g_uMsgShutdown = RegisterWindowMessageW(TVC_MSG_SHUTDOWN);
    g_uMsgRefresh = RegisterWindowMessageW(TVC_MSG_REFRESH);

    g_bMessagesReady = true;
}

// ---------------------------------------------------------------------------- raw input

static void StartListening()
{
    if (!g_hIconWnd || !IsWindow(g_hIconWnd)) return;

    RAWINPUTDEVICE rid = {
        HID_USAGE_PAGE_GENERIC,
        HID_USAGE_GENERIC_MOUSE,
        RIDEV_INPUTSINK,
        g_hIconWnd
    };

    // The mouse can only be registered to one window per process, so re-asserting this on
    // every handshake also takes the stream back if anything else loaded into explorer.exe
    // registered it.
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
}

static void StopListening()
{
    RAWINPUTDEVICE rid = {
        HID_USAGE_PAGE_GENERIC,
        HID_USAGE_GENERIC_MOUSE,
        RIDEV_REMOVE
    };
    RegisterRawInputDevices(&rid, 1, sizeof(rid));

    g_hIconWnd = NULL;
    g_hHostWnd = NULL;
}

static bool GetTrayIconRect(RECT* pRect)
{
    NOTIFYICONIDENTIFIER niid;
    ZeroMemory(&niid, sizeof(NOTIFYICONIDENTIFIER));
    niid.cbSize = sizeof(NOTIFYICONIDENTIFIER);
    niid.hWnd = g_hIconWnd;
    niid.uID = UID_TRAYICONVOLUME;
    niid.guidItem = GUID_TRAYICONVOLUME;

    if (SUCCEEDED(Shell_NotifyIconGetRect(&niid, pRect)))
    {
        g_rcIconCached = *pRect;
        g_dwIconCachedTick = GetTickCount();
        return true;
    }

    // The shell can fail this while the taskbar slides in or out. The last known rect keeps
    // a notch from being swallowed.
    if (g_dwIconCachedTick && (GetTickCount() - g_dwIconCachedTick) < 10000)
    {
        *pRect = g_rcIconCached;
        return true;
    }

    return false;
}

static bool IsCursorOnTrayIcon()
{
    POINT ptCursor;
    RECT rcIcon;

    if (!GetCursorPos(&ptCursor)) return false;
    if (!GetTrayIconRect(&rcIcon)) return false;

    return PtInRect(&rcIcon, ptCursor) != 0;
}

// Windows 10's volume flyout is a XAML island hosted by ShellExperienceHost.exe. While it is
// open it takes the foreground and handles the wheel itself, so forwarding the same notch
// would apply the step twice. Asking for the current foreground window instead of
// remembering the last click means this can never get stuck.
static bool IsShellFlyoutOpen()
{
    HWND hForeground = GetForegroundWindow();
    if (!hForeground) return false;

    wchar_t szClass[64];
    if (!GetClassNameW(hForeground, szClass, ARRAYSIZE(szClass))) return false;
    if (lstrcmpW(szClass, L"Windows.UI.Core.CoreWindow") != 0) return false;

    DWORD dwProcessId = 0;
    GetWindowThreadProcessId(hForeground, &dwProcessId);
    if (!dwProcessId) return false;

    bool bIsFlyoutHost = false;
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, dwProcessId);
    if (hProcess)
    {
        wchar_t szPath[MAX_PATH];
        DWORD cchPath = ARRAYSIZE(szPath);
        if (QueryFullProcessImageNameW(hProcess, 0, szPath, &cchPath))
        {
            LPCWSTR szName = wcsrchr(szPath, L'\\');
            szName = szName ? szName + 1 : szPath;
            bIsFlyoutHost = (lstrcmpiW(szName, L"ShellExperienceHost.exe") == 0);
        }
        CloseHandle(hProcess);
    }

    return bIsFlyoutHost;
}

static void OnRawInput(LPARAM lParam)
{
    if (!g_hHostWnd || !g_hIconWnd) return;

    RAWINPUT raw;
    ZeroMemory(&raw, sizeof(RAWINPUT));

    UINT cbSize = sizeof(RAWINPUT);
    if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, &raw, &cbSize, sizeof(RAWINPUTHEADER)) == (UINT)-1) return;
    if (raw.header.dwType != RIM_TYPEMOUSE) return;
    if ((raw.data.mouse.usButtonFlags & RI_MOUSE_WHEEL) != RI_MOUSE_WHEEL) return;

    if (!IsCursorOnTrayIcon()) return;
    if (IsShellFlyoutOpen()) return;

    // One detent is WHEEL_DELTA; high resolution wheels and fast flicks report multiples of
    // it, and all of those notches have to reach the host.
    int nSteps = (short)raw.data.mouse.usButtonData / WHEEL_DELTA;
    if (nSteps == 0) nSteps = ((short)raw.data.mouse.usButtonData > 0) ? 1 : -1;

    PostMessageW(g_hHostWnd, g_uMsgWheel, (WPARAM)nSteps, 0);
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

// Posted messages: raw input is delivered that way, and a WH_CALLWNDPROC hook never sees it.
extern "C" __declspec(dllexport) LRESULT CALLBACK GetMsgProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    EnsureMessages();

    if (nCode == HC_ACTION && wParam == PM_REMOVE && g_uMsgWheel)
    {
        MSG* pMsg = (MSG*)lParam;
        if (pMsg && pMsg->message == WM_INPUT)
        {
            OnRawInput(pMsg->lParam);
        }
    }

    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// Sent messages: the host's handshake and the tray's own callback for the icon.
extern "C" __declspec(dllexport) LRESULT CALLBACK CallWndProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    EnsureMessages();

    if (nCode >= HC_ACTION && g_uMsgInit)
    {
        LPCWPSTRUCT cwps = (LPCWPSTRUCT)lParam;

        if (cwps->message == g_uMsgInit)
        {
            g_hIconWnd = (HWND)cwps->wParam;
            g_hHostWnd = (HWND)cwps->lParam;

            StartListening();

            if (g_hHostWnd) PostMessageW(g_hHostWnd, g_uMsgAlive, 0, 0);
        }
        else if (cwps->message == g_uMsgShutdown)
        {
            StopListening();
        }
        else if (cwps->message == g_uMsgRefresh)
        {
            RefreshTrayTooltip();
        }
        else if (cwps->message == WM_TRAYICONVOLUME)
        {
            // Fallback bootstrap in case the handshake never got through.
            if (!g_hIconWnd)
            {
                g_hIconWnd = cwps->hwnd;
                StartListening();
            }

            if (LOWORD(cwps->lParam) == WM_MBUTTONUP && g_hHostWnd)
            {
                PostMessageW(g_hHostWnd, g_uMsgMute, 0, 0);
            }
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
    // Nothing to do, on purpose. The host sends TVC_MSG_SHUTDOWN before it unhooks, and if
    // the host is killed instead it simply stops feeding us: the raw input registration left
    // behind belongs to a window of the shell itself, so it stays harmless.
    return TRUE;
}
