@echo off
rem x64 only. The 32-bit build was retired on 2026-09-30: the shipped runtime is
rem x64/shadowtrainer.dll only, and no x86 test target is built or kept.
setlocal
set "ARCH=x64"
set "VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community"
if not exist "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" exit /b 3
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 exit /b %errorlevel%
pushd "%~dp0"
if not exist "build\x64" mkdir "build\x64"
if not exist "dist\x64" mkdir "dist\x64"
rem src/ is split into modules, so each one is its own include root and the
rem sources keep saying #include "core.hpp" instead of naming a directory.
set "FLAGS=/nologo /std:c++20 /EHsc /MT /W4 /utf-8 /O2 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /Iinclude /Isrc /Isrc\core /Isrc\ui /Isrc\bridge"
set "CORE_SOURCES=src\core\core.cpp src\core\typed_value.cpp src\core\typed_scan.cpp"
set "WEBVIEW2_INCLUDE=/Ithird_party\webview2\include"
set "WEBVIEW2_LIB=third_party\webview2\lib\x64\WebView2LoaderStatic.lib"
rem Web UI assets are embedded as RCDATA so the shipped DLL needs no files on
rem disk. rc.exe resolves the RCDATA file names through /I (verified: it ignores
rem the working directory), so this line behaves identically here and under a
rem Visual Studio build of shadowtrainer.vcxproj.
rc /nologo /Iweb /Fo"build\x64\webui.res" web\webui.rc
if errorlevel 1 goto failed
cl %FLAGS% %WEBVIEW2_INCLUDE% /Iweb /DSHADOWTRAINER_BUILD /LD src\runtime.cpp %CORE_SOURCES% src\ui\cover.cpp src\ui\webui.cpp src\ui\webview_host.cpp src\bridge\ui_bridge.cpp /Fobuild\x64\ /Fedist\x64\shadowtrainer.dll /link /INCREMENTAL:NO /DYNAMICBASE /NXCOMPAT /IMPLIB:build\x64\shadowtrainer.lib user32.lib gdi32.lib comdlg32.lib msimg32.lib xmllite.lib shlwapi.lib ole32.lib dwmapi.lib advapi32.lib %WEBVIEW2_LIB% build\x64\webui.res
if errorlevel 1 goto failed
rem Tests and development tools are build inputs, not build outputs: compile them
rem when the directories are there. Guarded so a partial checkout still produces
rem the DLL instead of failing on a missing source file.
if exist "tests\core_tests.cpp" (
cl %FLAGS% tests\core_tests.cpp %CORE_SOURCES% /Fobuild\x64\ /Febuild\x64\core_tests.exe /link xmllite.lib shlwapi.lib ole32.lib
if errorlevel 1 goto failed
cl %FLAGS% tests\runtime_tests.cpp /Fobuild\x64\ /Febuild\x64\runtime_tests.exe /link user32.lib
if errorlevel 1 goto failed
cl %FLAGS% tests\typed_tests.cpp %CORE_SOURCES% /Fobuild\x64\ /Febuild\x64\typed_tests.exe /link xmllite.lib shlwapi.lib ole32.lib
if errorlevel 1 goto failed
cl %FLAGS% tests\hotkey_tests.cpp /Fobuild\x64\ /Febuild\x64\hotkey_tests.exe
if errorlevel 1 goto failed
cl %FLAGS% tests\pointer_scan_tests.cpp %CORE_SOURCES% /Fobuild\x64\ /Febuild\x64\pointer_scan_tests.exe /link xmllite.lib shlwapi.lib ole32.lib
if errorlevel 1 goto failed
cl %FLAGS% tests\runtime_v2_tests.cpp /Fobuild\x64\ /Febuild\x64\runtime_v2_tests.exe /link user32.lib
if errorlevel 1 goto failed
rem The web UI's browser-free half, exercised headlessly (mode A) and against
rem the real DLL (mode B).
cl %FLAGS% tests\bridge_tests.cpp src\bridge\ui_bridge.cpp /Fobuild\x64\ /Febuild\x64\bridge_tests.exe /link xmllite.lib shlwapi.lib ole32.lib
if errorlevel 1 goto failed
cl %FLAGS% tests\polish_core_tests.cpp %CORE_SOURCES% /Fobuild\x64\ /Febuild\x64\polish_core_tests.exe /link xmllite.lib shlwapi.lib ole32.lib
if errorlevel 1 goto failed
)
if exist "tools\dev_host.cpp" (
rem Development host: loads the DLL in-process so web/ can be edited and reloaded
rem without a rebuild. A tool, not a product.
cl %FLAGS% tools\dev_host.cpp /Fobuild\x64\ /Febuild\x64\dev_host.exe /link /SUBSYSTEM:CONSOLE
if errorlevel 1 goto failed
rem Debug-output tap: prints the OutputDebugStringW traffic that src/ui/webui.cpp
rem emits. Also a tool, not a product.
cl %FLAGS% tools\dbwin_capture.cpp /Fobuild\x64\ /Febuild\x64\dbwin_capture.exe /link /SUBSYSTEM:CONSOLE
if errorlevel 1 goto failed
)
dumpbin /dependents dist\x64\shadowtrainer.dll > build\x64\dependencies.txt
if errorlevel 1 goto failed
rem The DLL is injected into an arbitrary host process, so the WebView2 loader has
rem to be linked in statically: a side-by-side WebView2Loader.dll cannot be relied
rem on. Fail loudly rather than shipping a DLL that cannot find its loader.
findstr /C:"WebView2Loader.dll" build\x64\dependencies.txt >nul
if not errorlevel 1 (
  echo ERROR: shadowtrainer.dll imports WebView2Loader.dll; it must link WebView2LoaderStatic.lib
  goto failed
)
dumpbin /exports dist\x64\shadowtrainer.dll > build\x64\exports.txt
if errorlevel 1 goto failed
popd
exit /b 0
:failed
set "RESULT=%errorlevel%"
popd
exit /b %RESULT%
