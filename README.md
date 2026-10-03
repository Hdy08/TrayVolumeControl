# TrayVolumeControl

This project implements ability to scroll the volume icon from tray to regulate volume level from Windows 11.
It is also possible to middle click on the volume icon to toggle Mute mode.

Mouse input is received by TrayVolumeControl itself, independently of Explorer's input registration and the foreground window. Opening or closing the volume flyout, opening the Start menu, and revealing an auto-hidden taskbar do not suspend scrolling or middle-click mute. Input is handled only while the pointer is over the visible volume icon; the injected DLL is used only to refresh its tooltip.

To test a new build, exit any running copy first, then run `TrayVolumeControl.exe` with the matching `TrayVolumeControlLib.dll` in the same directory. Check scrolling and middle-click mute after closing the volume flyout and after pressing the Windows key. Also check that scrolling outside the volume icon does not change the volume.

[Visual C++ Redistributable](https://docs.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-170) is required.

This should work on Windows 7 and higher.

![Demo](https://raw.githubusercontent.com/krlvm/TrayVolumeControl/master/demo.gif)
