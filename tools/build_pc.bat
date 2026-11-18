@echo off
rem Builds the PC VR version: build\pc\ProjectArena\ProjectArena.exe + assets\ (the open asset set).
rem Needs Visual Studio 2022 (C++ workload). Headers: OpenXR from third_party\openxr_aar, GLES 3 from the Android NDK.
setlocal
set ROOT=%~dp0..
set OUT=%ROOT%\build\pc\ProjectArena
set INC=%ROOT%\build\pc\include
set NDKINC=C:\Android\sdk\ndk\26.3.11579264\toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\include
set OXR=%ROOT%\third_party\openxr_aar\prefab\modules\headers\include
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%INC%\GLES3" mkdir "%INC%\GLES3"
if not exist "%INC%\KHR" mkdir "%INC%\KHR"
copy /y "%NDKINC%\GLES3\gl3.h" "%INC%\GLES3\" >nul || exit /b 1
copy /y "%NDKINC%\GLES3\gl3platform.h" "%INC%\GLES3\" >nul || exit /b 1
copy /y "%NDKINC%\KHR\khrplatform.h" "%INC%\KHR\" >nul || exit /b 1
python -I "%ROOT%\tools\write_servers_header.py" || exit /b 1
rem the sources: app\src, or PA_SRC: a release's, the public form of the source (tools\public_source.py, without
rem the private cross-play code; the owner, 2026-11-18)
if not defined PA_SRC set "PA_SRC=%ROOT%\app\src"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%ROOT%\build\pc\obj" mkdir "%ROOT%\build\pc\obj"
rem the icon (taskbar, title bar, Explorer): every build embeds app\src\pc\ProjectArena.ico
rc /nologo /fo "%ROOT%\build\pc\obj\ProjectArena.res" "%ROOT%\app\src\pc\ProjectArena.rc" || exit /b 1
set XP_SRC=
cl /nologo /std:c++17 /O2 /EHsc /W3 /MT /D_CRT_SECURE_NO_WARNINGS /D_WINSOCK_DEPRECATED_NO_WARNINGS /DPROJECTARENA_QUIET ^
   /I"%PA_SRC%" /I"%INC%" /I"%OXR%" /Fo"%ROOT%\build\pc\obj\\" /Fe"%OUT%\ProjectArena.exe" ^
   "%PA_SRC%\main.cpp" "%PA_SRC%\render.cpp" "%PA_SRC%\audio.cpp" "%PA_SRC%\game.cpp" ^
   "%PA_SRC%\net.cpp" "%PA_SRC%\record.cpp" "%PA_SRC%\training.cpp" "%PA_SRC%\sound_studio.cpp" "%PA_SRC%\entitlement.cpp" %XP_SRC% "%PA_SRC%\avatar.cpp" "%PA_SRC%\log.cpp" "%PA_SRC%\platform.cpp" ^
   "%PA_SRC%\pc\gl_pc.cpp" "%PA_SRC%\pc\xr_loader.cpp" "%PA_SRC%\pc\discord_rpc.cpp" "%PA_SRC%\pc\webcam.cpp" "%PA_SRC%\pc\updater.cpp" "%ROOT%\build\pc\obj\ProjectArena.res" ^
   /link /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup opengl32.lib user32.lib gdi32.lib ws2_32.lib ole32.lib shell32.lib advapi32.lib hid.lib setupapi.lib mfplat.lib mf.lib mfreadwrite.lib mfuuid.lib comctl32.lib winhttp.lib bcrypt.lib || exit /b 1
robocopy "%ROOT%\app\assets_open" "%OUT%\assets" /MIR /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 exit /b 1
rem the licensed music (app\assets_music: Suno, used under its terms, not CC BY), when there is any
if exist "%ROOT%\app\assets_music\assets\music\*.wav" (
  robocopy "%ROOT%\app\assets_music\assets\music" "%OUT%\assets\music" *.wav events.txt /NFL /NDL /NJH /NJS /NP >nul
  if errorlevel 8 exit /b 1
)
copy /y "%ROOT%\LICENSE" "%OUT%\LICENSE.txt" >nul
copy /y "%ROOT%\NOTICE.txt" "%OUT%\NOTICE.txt" >nul
copy /y "%ROOT%\docs\PC_README.txt" "%OUT%\README.txt" >nul
echo built %OUT%\ProjectArena.exe
exit /b 0
