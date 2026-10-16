@echo off
rem Builds the desktop avatar draw-list helper used by tools/preview_avatar.py (Visual Studio 2022 toolchain).
setlocal
set ROOT=%~dp0..
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%ROOT%\build\tests" mkdir "%ROOT%\build\tests"
cl /nologo /std:c++17 /O2 /EHsc /W3 /Fe"%ROOT%\build\tests\avatar_pose.exe" /Fo"%ROOT%\build\tests\\" "%ROOT%\tools\avatar_pose.cpp" "%ROOT%\app\src\avatar.cpp" || exit /b 1
