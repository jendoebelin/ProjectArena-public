@echo off
rem Builds the arena room test (net.cpp on Winsock). Run it with: python -I tools/run_room_test.py
setlocal
set ROOT=%~dp0..
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%ROOT%\build\tests" mkdir "%ROOT%\build\tests"
cl /nologo /std:c++17 /O2 /EHsc /W3 /D_WINSOCK_DEPRECATED_NO_WARNINGS /Fe"%ROOT%\build\tests\test_rooms.exe" /Fo"%ROOT%\build\tests\\" "%ROOT%\app\tests\test_rooms.cpp" "%ROOT%\app\src\net.cpp" "%ROOT%\app\src\game.cpp" "%ROOT%\app\src\avatar.cpp" "%ROOT%\app\src\log.cpp" ws2_32.lib >nul || exit /b 1
