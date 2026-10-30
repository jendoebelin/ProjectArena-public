@echo off
rem Builds and runs the desktop gameplay tests with the Visual Studio 2022 toolchain.
setlocal
set ROOT=%~dp0..
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%ROOT%\build\tests" mkdir "%ROOT%\build\tests"
cl /nologo /std:c++17 /O2 /EHsc /W3 /Fe"%ROOT%\build\tests\test_game.exe" /Fo"%ROOT%\build\tests\\" "%ROOT%\app\tests\test_game.cpp" "%ROOT%\app\src\game.cpp" "%ROOT%\app\src\avatar.cpp" "%ROOT%\app\src\record.cpp" "%ROOT%\app\src\training.cpp" /link /STACK:16777216 || exit /b 1
"%ROOT%\build\tests\test_game.exe" "%ROOT%\app\assets_open\arena.col" || exit /b 1
rem the match the tests recorded (RECORD MY MATCHES): whole, and readable by the analyzer
python -I "%ROOT%\tools\analyze_play.py" --check "%TEMP%\projectarena_sample_match.par" || exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /Fe"%ROOT%\build\tests\test_xcompat.exe" /Fo"%ROOT%\build\tests\\" "%ROOT%\app\tests\test_xcompat.cpp" "%ROOT%\app\src\xcompat.cpp" "%ROOT%\app\src\log.cpp" ws2_32.lib || exit /b 1
if exist "%ROOT%\build\xc_capture.bin" ("%ROOT%\build\tests\test_xcompat.exe" "%ROOT%\build\xc_capture.bin") else ("%ROOT%\build\tests\test_xcompat.exe")
