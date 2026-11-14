@echo off
rem Builds the reach probe (catches in bot matches, the knuckle box, backhands). Run it with: python -I tools/run_reach_probe.py
setlocal
set ROOT=%~dp0..
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%ROOT%\build\tests\reach" mkdir "%ROOT%\build\tests\reach"
cl /nologo /std:c++17 /O2 /EHsc /W3 /Fe"%ROOT%\build\tests\reach_probe.exe" /Fo"%ROOT%\build\tests\reach\\" "%ROOT%\app\tests\reach_probe.cpp" "%ROOT%\app\src\game.cpp" "%ROOT%\app\src\avatar.cpp" "%ROOT%\app\src\record.cpp" "%ROOT%\app\src\training.cpp" /link /STACK:16777216 || exit /b 1
echo built build\tests\reach_probe.exe
