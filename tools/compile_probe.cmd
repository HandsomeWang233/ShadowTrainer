@echo off
rem Compile one self-contained probe/test source by hand, without touching
rem build.cmd or the vcxproj source lists. The source must stand alone: this
rem only ever passes a single .cpp to cl, so anything needing core.cpp and
rem friends (see CORE_SOURCES in build.cmd) will not link here.
rem
rem Usage: tools\compile_probe.cmd hotkey_tests      -> build\x64\hotkey_tests.exe
rem
rem Lives in tools\ rather than build\ because build\ is disposable output:
rem anything kept there is lost the next time that tree is cleaned.
setlocal
if "%~1"=="" (
  >&2 echo Usage: compile_probe.cmd ^<source name without .cpp^>
  exit /b 2
)
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if errorlevel 1 exit /b %errorlevel%
rem Paths inside cl are relative, so run from the repo root regardless of the
rem caller's working directory.
pushd "%~dp0.."
cl /nologo /std:c++20 /EHsc /MT /W3 /utf-8 /O2 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /Iinclude /Isrc /Isrc\core /Isrc\ui /Isrc\bridge tests\%1.cpp /Fobuild\x64\ /Febuild\x64\%1.exe /link user32.lib gdi32.lib comctl32.lib comdlg32.lib xmllite.lib shlwapi.lib ole32.lib msimg32.lib dwmapi.lib gdiplus.lib
set "RESULT=%errorlevel%"
popd
exit /b %RESULT%
