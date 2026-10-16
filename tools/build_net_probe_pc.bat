@echo off
rem Builds app/tests/net_probe.cpp for Windows -> build\tests\net_probe.exe (a PC stand-in for a headset).
setlocal
set ROOT=%~dp0..
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /W3 /D_WINSOCK_DEPRECATED_NO_WARNINGS /D_CRT_SECURE_NO_WARNINGS /Fe"%ROOT%\build\tests\net_probe.exe" /Fo"%ROOT%\build\tests\\" "%ROOT%\app\tests\net_probe.cpp" "%ROOT%\app\src\net.cpp" "%ROOT%\app\src\game.cpp" "%ROOT%\app\src\avatar.cpp" "%ROOT%\app\src\log.cpp" ws2_32.lib >nul || exit /b 1
echo built build\tests\net_probe.exe
