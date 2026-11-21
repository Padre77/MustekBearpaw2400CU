@echo off
setlocal
if not "%~1"=="" if /i not "%~1"=="--tools-only" (
    echo Usage: build.cmd [--tools-only] 1>&2
    exit /b 2
)
cd /d "%~dp0" || exit /b 1
if "%~1"=="" if not exist "Driver\PS2fw.usb" (
    echo WIA build requires Driver\PS2fw.usb. See README.md or use build.cmd --tools-only. 1>&2
    exit /b 1
)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio Build Tools with the C++ workload are required. 1>&2
    exit /b 1
)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
    echo No Visual Studio C++ toolset found. 1>&2
    exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
if not exist build mkdir build || exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /Fo:build\UsbProbe.obj /Fe:build\BearPaw2400CUProbe.exe UsbProbe.cpp /link cfgmgr32.lib || exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /Fo:build\WinUsbStatus.obj /Fe:build\BearPaw2400CUStatus.exe WinUsbStatus.cpp /link bcrypt.lib cfgmgr32.lib winusb.lib || exit /b 1
cl /nologo /DUNICODE /std:c++17 /EHsc /W4 /WX /O2 /MT /Fo:build\BindWinUsb.obj /Fe:build\BindWinUsb.exe BindWinUsb.cpp /link cfgmgr32.lib setupapi.lib newdev.lib advapi32.lib || exit /b 1
"%SystemRoot%\Microsoft.NET\Framework64\v4.0.30319\csc.exe" /nologo /warnaserror /codepage:65001 /platform:x64 /target:winexe /optimize+ /out:build\BearPaw2400CU.exe ScannerWindow.cs || exit /b 1
if /i "%~1"=="--tools-only" exit /b 0
rc /nologo /fobuild\WiaFirmware.res WiaFirmware.rc || exit /b 1
cl /nologo /c /DBEARPAW_WIA_BUILD /std:c++17 /EHsc /W4 /WX /wd4505 /O2 /MT /Fo:build\WiaTransport.obj WinUsbStatus.cpp || exit /b 1
cl /nologo /LD /std:c++17 /EHsc /W4 /WX /O2 /MT /Fo:build\WiaMicrodriver.obj /Fe:build\BearPaw2400CUWia.dll WiaMicrodriver.cpp build\WiaTransport.obj build\WiaFirmware.res /link /DEF:BearPaw2400CUWia.def bcrypt.lib cfgmgr32.lib winusb.lib
exit /b %errorlevel%
