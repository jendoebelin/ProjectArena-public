@echo off
rem BALL IN FLIGHT, to listen to: builds app\tests\flight_demo.cpp and renders a few balls flown past a still listener
rem to build\flight_demo\ball_flight_demo.wav (the game's own sound, app\src\flight_sound.h), with their loudness printed
setlocal
set ROOT=%~dp0..
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%ROOT%\build\flight_demo" mkdir "%ROOT%\build\flight_demo"
cl /nologo /std:c++17 /O2 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /Fe"%ROOT%\build\flight_demo\flight_demo.exe" /Fo"%ROOT%\build\flight_demo\\" "%ROOT%\app\tests\flight_demo.cpp" >nul || exit /b 1
"%ROOT%\build\flight_demo\flight_demo.exe" "%ROOT%\build\flight_demo\ball_flight_demo.wav"
