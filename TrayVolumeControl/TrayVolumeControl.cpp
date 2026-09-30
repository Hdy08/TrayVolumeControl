#include <iostream>
#include <Windows.h>
#include <CommCtrl.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include "../TrayVolumeControlLib/TVCShared.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")

const LPCWSTR szWindowClass = L"TRAYVOLCTRL";
const LPCWSTR szLibraryName = L"TrayVolumeControlLib.dll";
const UINT WM_TASKBARCREATED = RegisterWindowMessage(L"TaskbarCreated");

const UINT_PTR TIMER_HEALTH = 1;
const UINT HEALTH_INTERVAL_MS = 5000;
const DWORD ALIVE_TIMEOUT_MS = 30000;
const int KEEPALIVE_EVERY_N_TICKS = 12;      // re-assert the DLL registration once a minute

// Volume step applied per one mouse wheel notch (0.02 = 2%)
const float fVolumeStep = 0.02f;

static UINT g_uMsgInit = 0;
static UINT g_uMsgAlive = 0;
static UINT g_uMsgWheel = 0;
static UINT g_uMsgMute = 0;
static UINT g_uMsgShutdown = 0;
static UINT g_uMsgRefresh = 0;

static HHOOK g_hCallWndHook = NULL;
static HHOOK g_hGetMsgHook = NULL;
static HINSTANCE g_hLibrary = NULL;
static HWND g_hIconWnd = NULL;
static DWORD g_dwIconThread = 0;
static HWND g_hHostWnd = NULL;
static DWORD g_dwLastAlive = 0;
static int g_nHealthTicks = 0;

// The volume is changed on a worker thread: the audio service can take seconds to answer
// while the default device is switching, and the window procedure must stay responsive.
static HANDLE g_hVolumeEvent = NULL;
static HANDLE g_hVolumeThread = NULL;
static volatile LONG g_nPendingSteps = 0;
static volatile LONG g_bPendingMute = 0;
static volatile LONG g_bWorkerStop = 0;

struct TRAYDATA
{
	HWND hwnd;
	UINT uID;
	UINT uCallbackMessage;
	DWORD Reserved[2];
	HICON hIcon;
};

// ---------------------------------------------------------------------------- hook library

// The DLL sits next to the executable. Resolving it relative to the working directory
// (as older builds did) silently fails whenever the process is started with another
// working directory.
HINSTANCE LoadHookLibrary()
{
	wchar_t szPath[MAX_PATH];
	DWORD cchLength = GetModuleFileNameW(NULL, szPath, ARRAYSIZE(szPath));

	if (cchLength > 0 && cchLength < ARRAYSIZE(szPath))
	{
		wchar_t* pszFileName = wcsrchr(szPath, L'\\');
		if (pszFileName && (pszFileName - szPath + 1 + lstrlenW(szLibraryName)) < ARRAYSIZE(szPath))
		{
			*(pszFileName + 1) = L'\0';
			lstrcatW(szPath, szLibraryName);

			HINSTANCE hLibrary = LoadLibraryW(szPath);
			if (hLibrary) return hLibrary;
		}
	}

	return LoadLibraryW(szLibraryName);
}

// Finds the volume icon in the tray toolbar and reports the window owning it: that window is
// the one the tray sends its callback messages to and the one that has to receive the raw
// input used to read the wheel.
bool FindVolumeTrayIcon(HWND* phIconWnd, DWORD* pdwThreadId)
{
	HWND hWndTray = FindTrayToolbarWindow();
	if (!hWndTray) return false;

	// Timeout guarded: the taskbar belongs to another process, and a hung shell must not
	// freeze (or outlive) this one.
	DWORD_PTR dwCount = 0;
	if (!SendMessageTimeoutW(hWndTray, TB_BUTTONCOUNT, 0, 0, SMTO_ABORTIFHUNG, 1000, &dwCount))
		return false;

	int count = (int)dwCount;
	if (count <= 0) return false;

	DWORD dwProcessID = 0;
	GetWindowThreadProcessId(hWndTray, &dwProcessID);

	HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, dwProcessID);
	if (!hProcess) return false;

	bool bFound = false;

	for (int i = count - 1; i >= 0 && !bFound; i--)
	{
		size_t dwBytesRead = 0;

		TBBUTTON tbButton;
		ZeroMemory(&tbButton, sizeof(TBBUTTON));
		void* lpButton = VirtualAllocEx(hProcess, NULL, sizeof(TBBUTTON), MEM_COMMIT, PAGE_READWRITE);
		if (!lpButton) continue;

		if (WriteProcessMemory(hProcess, lpButton, &tbButton, sizeof(TBBUTTON), &dwBytesRead))
		{
			DWORD_PTR dwResult = 0;
			if (!SendMessageTimeoutW(hWndTray, TB_GETBUTTON, i, (LPARAM)lpButton, SMTO_ABORTIFHUNG, 1000, &dwResult))
			{
				VirtualFreeEx(hProcess, lpButton, 0, MEM_RELEASE);
				continue;
			}

			TRAYDATA trayData;
			ZeroMemory(&trayData, sizeof(TRAYDATA));

			if (ReadProcessMemory(hProcess, lpButton, &tbButton, sizeof(TBBUTTON), &dwBytesRead)
				&& ReadProcessMemory(hProcess, (void*)tbButton.dwData, &trayData, sizeof(TRAYDATA), &dwBytesRead)
				&& trayData.uID == UID_TRAYICONVOLUME)
			{
				*phIconWnd = trayData.hwnd;
				*pdwThreadId = GetWindowThreadProcessId(trayData.hwnd, NULL);
				bFound = (*pdwThreadId != 0);
			}
		}

		VirtualFreeEx(hProcess, lpButton, 0, MEM_RELEASE);
	}

	CloseHandle(hProcess);

	return bFound;
}

// ---------------------------------------------------------------------------- audio

IAudioEndpointVolume* GetDefaultEndpointVolume()
{
	IMMDeviceEnumerator* pEnumerator = NULL;
	HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_INPROC_SERVER,
		__uuidof(IMMDeviceEnumerator), (LPVOID*)&pEnumerator);
	if (FAILED(hr)) return NULL;

	// Always the current default device: caching this would keep writing to a device the
	// user has already switched away from.
	IMMDevice* pDevice = NULL;
	hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
	pEnumerator->Release();
	if (FAILED(hr)) return NULL;

	IAudioEndpointVolume* pVolume = NULL;
	hr = pDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, NULL, (LPVOID*)&pVolume);
	pDevice->Release();
	if (FAILED(hr)) return NULL;

	return pVolume;
}

// Asks the hook DLL to re-show the tray tooltip, so the text above the volume icon follows
// the level while scrolling. Runs on the volume worker thread: the sleep and the cross
// process send must never touch the window thread.
void RefreshTrayTooltip()
{
	if (!g_hIconWnd || !g_uMsgRefresh) return;

	// The shell refreshes the tooltip text itself once it has seen the new level.
	Sleep(40);

	DWORD_PTR dwResult = 0;
	SendMessageTimeoutW(g_hIconWnd, g_uMsgRefresh, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 300, &dwResult);
}

void ChangeVolume(int nSteps)
{
	IAudioEndpointVolume* pVolume = GetDefaultEndpointVolume();
	if (!pVolume) return;

	float fCurrentVolume = 0;
	if (SUCCEEDED(pVolume->GetMasterVolumeLevelScalar(&fCurrentVolume)))
	{
		float fNewVolume = max(0, min(1.0f, fCurrentVolume + fVolumeStep * nSteps));

		if (fCurrentVolume != fNewVolume)
		{
			BOOL bIsMute = FALSE;
			if (SUCCEEDED(pVolume->GetMute(&bIsMute)))
			{
				// Reaching 0% mutes, scrolling up from a muted state unmutes.
				if (!bIsMute && fNewVolume < fVolumeStep * 0.5f)
				{
					pVolume->SetMute(true, NULL);
				}
				else if (bIsMute && fNewVolume > 0)
				{
					pVolume->SetMute(false, NULL);
				}
			}

			if (SUCCEEDED(pVolume->SetMasterVolumeLevelScalar(fNewVolume, NULL)))
			{
				pVolume->Release();
				RefreshTrayTooltip();
				return;
			}
		}
	}

	pVolume->Release();
}

void ToggleMute()
{
	IAudioEndpointVolume* pVolume = GetDefaultEndpointVolume();
	if (!pVolume) return;

	BOOL bIsMute = FALSE;
	if (SUCCEEDED(pVolume->GetMute(&bIsMute)) && SUCCEEDED(pVolume->SetMute(!bIsMute, NULL)))
	{
		pVolume->Release();
		RefreshTrayTooltip();
		return;
	}

	pVolume->Release();
}

DWORD WINAPI VolumeWorker(LPVOID)
{
	// MTA: no message pump is needed for these calls.
	CoInitializeEx(NULL, COINIT_MULTITHREADED);

	while (!g_bWorkerStop)
	{
		WaitForSingleObject(g_hVolumeEvent, 200);

		// Rapid scrolling arrives as several requests; applying the sum keeps every notch.
		LONG nSteps = InterlockedExchange(&g_nPendingSteps, 0);
		LONG bMute = InterlockedExchange(&g_bPendingMute, 0);

		if (bMute) ToggleMute();
		if (nSteps) ChangeVolume((int)nSteps);
	}

	CoUninitialize();
	return 0;
}

// ---------------------------------------------------------------------------- hook control

// Hands the DLL the icon window and asks it to (re)initialize; its answer proves that the
// hook is still reaching it.
void PingHook()
{
	if (!g_hIconWnd || !g_hHostWnd || !g_uMsgInit) return;

	DWORD_PTR dwResult = 0;
	SendMessageTimeoutW(g_hIconWnd, g_uMsgInit, (WPARAM)g_hIconWnd, (LPARAM)g_hHostWnd,
		SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &dwResult);
}

void Unhook()
{
	if (g_hIconWnd && g_uMsgShutdown)
	{
		// Lets the DLL drop its raw input registration before it is unloaded.
		DWORD_PTR dwResult = 0;
		SendMessageTimeoutW(g_hIconWnd, g_uMsgShutdown, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &dwResult);
	}

	if (g_hGetMsgHook)
	{
		UnhookWindowsHookEx(g_hGetMsgHook);
		g_hGetMsgHook = NULL;
	}

	if (g_hCallWndHook)
	{
		UnhookWindowsHookEx(g_hCallWndHook);
		g_hCallWndHook = NULL;
	}

	g_hIconWnd = NULL;
	g_dwIconThread = 0;
}

bool InjectHook()
{
	HWND hIconWnd = NULL;
	DWORD dwIconThread = 0;
	if (!FindVolumeTrayIcon(&hIconWnd, &dwIconThread)) return false;

	if (g_hCallWndHook && g_hGetMsgHook && g_hIconWnd == hIconWnd && g_dwIconThread == dwIconThread)
	{
		PingHook();
		return true;
	}

	Unhook();

	if (!g_hLibrary)
	{
		g_hLibrary = LoadHookLibrary();
		if (!g_hLibrary) return false;
	}

	HOOKPROC hCallWndProc = (HOOKPROC)GetProcAddress(g_hLibrary, "CallWndProc");
	HOOKPROC hGetMsgProc = (HOOKPROC)GetProcAddress(g_hLibrary, "GetMsgProc");
	if (!hCallWndProc || !hGetMsgProc) return false;

	// WH_GETMESSAGE is what makes the wheel reliable: raw input is *posted*, so the
	// WH_CALLWNDPROC hook that reads the tray's callbacks never sees it.
	HHOOK hCallWndHook = SetWindowsHookEx(WH_CALLWNDPROC, hCallWndProc, g_hLibrary, dwIconThread);
	if (!hCallWndHook) return false;

	HHOOK hGetMsgHook = SetWindowsHookEx(WH_GETMESSAGE, hGetMsgProc, g_hLibrary, dwIconThread);
	if (!hGetMsgHook)
	{
		UnhookWindowsHookEx(hCallWndHook);
		return false;
	}

	g_hCallWndHook = hCallWndHook;
	g_hGetMsgHook = hGetMsgHook;
	g_hIconWnd = hIconWnd;
	g_dwIconThread = dwIconThread;
	g_dwLastAlive = GetTickCount();

	PingHook();

	return true;
}

// Keeps the hook in sync with the tray: the tray rebuilds its windows (shell restart,
// monitor or DPI change, theme change) and hooks can stop being delivered, so the
// installation is verified instead of being trusted once.
void CheckHealth()
{
	g_nHealthTicks++;

	if (!g_hCallWndHook || !g_hGetMsgHook || !g_hIconWnd || !IsWindow(g_hIconWnd)
		|| (GetTickCount() - g_dwLastAlive) > ALIVE_TIMEOUT_MS)
	{
		InjectHook();
		return;
	}

	if ((g_nHealthTicks % KEEPALIVE_EVERY_N_TICKS) == 0)
	{
		PingHook();
	}
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	LRESULT lResult = DefWindowProc(hWnd, message, wParam, lParam);

	if (message == WM_TASKBARCREATED)
	{
		Sleep(2500);
		InjectHook();
	}
	else if (message == g_uMsgAlive)
	{
		g_dwLastAlive = GetTickCount();
	}
	else if (message == g_uMsgWheel)
	{
		InterlockedExchangeAdd(&g_nPendingSteps, (LONG)(INT_PTR)wParam);
		SetEvent(g_hVolumeEvent);
	}
	else if (message == g_uMsgMute)
	{
		InterlockedExchange(&g_bPendingMute, 1);
		SetEvent(g_hVolumeEvent);
	}
	else if (message == WM_TIMER && wParam == TIMER_HEALTH)
	{
		CheckHealth();
	}
	else if (message == WM_DISPLAYCHANGE || message == WM_SETTINGCHANGE || message == WM_DPICHANGED)
	{
		CheckHealth();
	}
	else if (message == WM_ENDSESSION || message == WM_DESTROY)
	{
		Unhook();
	}

	return lResult;
}

void StopVolumeWorker()
{
	if (!g_hVolumeThread) return;

	InterlockedExchange(&g_bWorkerStop, 1);
	SetEvent(g_hVolumeEvent);

	WaitForSingleObject(g_hVolumeThread, 3000);
	CloseHandle(g_hVolumeThread);
	g_hVolumeThread = NULL;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR    lpCmdLine,
	_In_ int       nCmdShow)
{
	HANDLE hMutex = CreateMutex(NULL, TRUE, szWindowClass);
	if (!hMutex || ERROR_ALREADY_EXISTS == GetLastError())
	{
		return ERROR_ALREADY_EXISTS;
	}

	g_hVolumeEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	g_hVolumeThread = CreateThread(NULL, 0, VolumeWorker, NULL, 0, NULL);

	WNDCLASSEX wcex = {};
	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.lpfnWndProc = WndProc;
	wcex.hInstance = hInstance;
	wcex.lpszClassName = szWindowClass;
	if (!RegisterClassEx(&wcex))
	{
		return 2;
	}

	// The window exists before hooking so the DLL has somewhere to answer.
	g_hHostWnd = CreateWindowEx(0, szWindowClass, nullptr, 0, 0, 0, 0, 0, nullptr, NULL, NULL, NULL);
	if (!g_hHostWnd)
	{
		return 3;
	}

	g_uMsgInit = RegisterWindowMessageW(TVC_MSG_INIT);
	g_uMsgAlive = RegisterWindowMessageW(TVC_MSG_ALIVE);
	g_uMsgWheel = RegisterWindowMessageW(TVC_MSG_WHEEL);
	g_uMsgMute = RegisterWindowMessageW(TVC_MSG_MUTE);
	g_uMsgShutdown = RegisterWindowMessageW(TVC_MSG_SHUTDOWN);
	g_uMsgRefresh = RegisterWindowMessageW(TVC_MSG_REFRESH);

	InjectHook();
	SetTimer(g_hHostWnd, TIMER_HEALTH, HEALTH_INTERVAL_MS, NULL);

	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	Unhook();
	StopVolumeWorker();

	ReleaseMutex(hMutex);
	CloseHandle(hMutex);

	return (int)msg.wParam;
}
