@echo off
rem Builds the long-distance test (net.cpp on Winsock). Run it with: python -I tools/run_lag_test.py
setlocal
set ROOT=%~dp0..
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%ROOT%\build\tests\lag" mkdir "%ROOT%\build\tests\lag"
cl /nologo /std:c++17 /O2 /EHsc /W3 /D_WINSOCK_DEPRECATED_NO_WARNINGS /Fe"%ROOT%\build\tests\lag_test.exe" /Fo"%ROOT%\build\tests\lag\\" "%ROOT%\app\tests\lag_test.cpp" "%ROOT%\app\src\net.cpp" "%ROOT%\app\src\game.cpp" "%ROOT%\app\src\avatar.cpp" "%ROOT%\app\src\log.cpp" ws2_32.lib || exit /b 1
echo built build\tests\lag_test.exe
