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
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%ROOT%\build\pc\obj" mkdir "%ROOT%\build\pc\obj"
rem the icon (taskbar, title bar, Explorer): every build embeds app\src\pc\ProjectArena.ico
rc /nologo /fo "%ROOT%\build\pc\obj\ProjectArena.res" "%ROOT%\app\src\pc\ProjectArena.rc" || exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /W3 /MT /D_CRT_SECURE_NO_WARNINGS /D_WINSOCK_DEPRECATED_NO_WARNINGS /DPROJECTARENA_QUIET ^
   /I"%ROOT%\app\src" /I"%INC%" /I"%OXR%" /Fo"%ROOT%\build\pc\obj\\" /Fe"%OUT%\ProjectArena.exe" ^
   "%ROOT%\app\src\main.cpp" "%ROOT%\app\src\render.cpp" "%ROOT%\app\src\audio.cpp" "%ROOT%\app\src\game.cpp" ^
   "%ROOT%\app\src\net.cpp" "%ROOT%\app\src\xcompat.cpp" "%ROOT%\app\src\avatar.cpp" "%ROOT%\app\src\log.cpp" "%ROOT%\app\src\platform.cpp" ^
   "%ROOT%\app\src\pc\gl_pc.cpp" "%ROOT%\app\src\pc\xr_loader.cpp" "%ROOT%\build\pc\obj\ProjectArena.res" ^
   /link /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup opengl32.lib user32.lib gdi32.lib ws2_32.lib ole32.lib shell32.lib advapi32.lib hid.lib setupapi.lib || exit /b 1
robocopy "%ROOT%\app\assets_open" "%OUT%\assets" /MIR /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 exit /b 1
copy /y "%ROOT%\LICENSE" "%OUT%\LICENSE.txt" >nul
copy /y "%ROOT%\NOTICE.txt" "%OUT%\NOTICE.txt" >nul
copy /y "%ROOT%\docs\PC_README.txt" "%OUT%\README.txt" >nul
echo built %OUT%\ProjectArena.exe
exit /b 0
