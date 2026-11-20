@echo off
rem The avatars' lighter triangle lists for players far away (tools\make_lods.cpp): builds the tool, then writes
rem app\assets_open\<model>.lod beside every avatar part (av_*.mdl) and gauntlet (gauntlet_*.mdl). Run it again after a
rem part changes. Optional: make_lods.bat --ratio 0.25 --error 0.004
setlocal
set ROOT=%~dp0..
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%ROOT%\build\tools" mkdir "%ROOT%\build\tools"
cl /nologo /std:c++17 /O2 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /Fe"%ROOT%\build\tools\make_lods.exe" /Fo"%ROOT%\build\tools\\" "%ROOT%\tools\make_lods.cpp" setargv.obj >nul || exit /b 1
rem (setargv.obj: the * in the names below expanded, as cmd leaves it to the program)
pushd "%ROOT%\app\assets_open"
"%ROOT%\build\tools\make_lods.exe" %* av_*.mdl gauntlet_*.mdl
set RC=%ERRORLEVEL%
popd
exit /b %RC%
