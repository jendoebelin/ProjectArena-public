@echo off
rem Builds the asset viewer's animation maker (tools/bot_anims.cpp: the bot's motion and the avatar rig) with Visual Studio 2022.
setlocal
set ROOT=%~dp0..
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%ROOT%\build\tests" mkdir "%ROOT%\build\tests"
cl /nologo /std:c++17 /O2 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /Fe"%ROOT%\build\tests\bot_anims.exe" /Fo"%ROOT%\build\tests\\" "%ROOT%\tools\bot_anims.cpp" "%ROOT%\app\src\game.cpp" "%ROOT%\app\src\avatar.cpp" "%ROOT%\app\src\record.cpp" "%ROOT%\app\src\training.cpp" /link /STACK:16777216 || exit /b 1
