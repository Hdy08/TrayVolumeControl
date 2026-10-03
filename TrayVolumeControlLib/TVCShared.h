#pragma once
#include <Windows.h>

// Session unique messages (RegisterWindowMessage). Fixed numbers in the private range are
// avoided on purpose: they would be sent to a window owned by the shell, where the same
// number may already mean something else.
//
// TVC_MSG_INIT:     host -> tray icon window. wParam = icon owner window, lParam = host
//                   window. Also used as a keep-alive for the tooltip hook.
// TVC_MSG_ALIVE:    DLL -> host, sent after TVC_MSG_INIT was handled.
// TVC_MSG_SHUTDOWN: host -> tray icon window, sent before the hook is removed.
// TVC_MSG_REFRESH:  host -> tray icon window, asks the DLL to re-show the tray tooltip so
//                   the text above the volume icon follows the level while scrolling.
#define TVC_MSG_INIT     L"TrayVolumeControl.Init"
#define TVC_MSG_ALIVE    L"TrayVolumeControl.Alive"
#define TVC_MSG_SHUTDOWN L"TrayVolumeControl.Shutdown"
#define TVC_MSG_REFRESH  L"TrayVolumeControl.Refresh"

#define UID_TRAYICONVOLUME  100
#define GUID_TRAYICONVOLUME { 0x7820AE73, 0x23E3, 0x4229, { 0x82, 0xC1, 0xE4, 0x1C, 0xB6, 0x7D, 0x5B, 0x9C } };
//                          {   7820AE73 -  23E3 -  4229  -   82    C1 -  E4    1C    B6    7D    5B    9C   }

HWND FindTrayToolbarWindow()
{
	HWND hWnd = FindWindow(L"Shell_TrayWnd", NULL);
	if (hWnd)
	{
		hWnd = FindWindowEx(hWnd, NULL, L"TrayNotifyWnd", NULL);
		if (hWnd)
		{
			hWnd = FindWindowEx(hWnd, NULL, L"SysPager", NULL);
			if (hWnd)
			{
				hWnd = FindWindowEx(hWnd, NULL, L"ToolbarWindow32", NULL);
			}
		}
	}
	return hWnd;
}
