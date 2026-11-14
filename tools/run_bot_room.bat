@echo off
rem Builds the BOT ROOM tool (app\tests\bot_room.cpp). Run it with: python -I tools/run_bot_room.py
setlocal
set ROOT=%~dp0..
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%ROOT%\build\tests\bot_room" mkdir "%ROOT%\build\tests\bot_room"
cl /nologo /std:c++17 /O2 /EHsc /W3 /D_WINSOCK_DEPRECATED_NO_WARNINGS /I"%ROOT%\app\src" /Fe"%ROOT%\build\tests\bot_room.exe" /Fo"%ROOT%\build\tests\bot_room\\" "%ROOT%\app\tests\bot_room.cpp" "%ROOT%\app\src\net.cpp" "%ROOT%\app\tests\bot_room_game.cpp" "%ROOT%\app\src\avatar.cpp" "%ROOT%\app\src\log.cpp" ws2_32.lib || exit /b 1
