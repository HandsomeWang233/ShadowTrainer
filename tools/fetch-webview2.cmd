@echo off
rem Re-fetch the vendored WebView2 SDK. third_party/webview2 is committed, so a
rem normal build never runs this; it exists so the vendored blobs have a
rem reproducible origin (this repo has no package manager).
rem
rem Usage: tools\fetch-webview2.cmd [version]      (default below)
setlocal
set "VERSION=%~1"
if "%VERSION%"=="" set "VERSION=1.0.4258.31"
set "ROOT=%~dp0.."
set "NUPKG=%TEMP%\microsoft.web.webview2.%VERSION%.nupkg"
set "URL=https://nuget.azure.cn/v3-flatcontainer/microsoft.web.webview2/%VERSION%/microsoft.web.webview2.%VERSION%.nupkg"

echo Fetching %URL%
curl -fsSL -o "%NUPKG%" "%URL%"
if errorlevel 1 (
  echo nuget.azure.cn failed; trying nuget.org
  set "URL=https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/%VERSION%"
  curl -fsSL -o "%NUPKG%" "%URL%"
  if errorlevel 1 exit /b 1
)

rem A .nupkg is a zip. Expand-Archive insists on a .zip extension.
copy /y "%NUPKG%" "%TEMP%\wv2.zip" >nul
powershell -NoProfile -Command "Expand-Archive -Force -LiteralPath '%TEMP%\wv2.zip' -DestinationPath '%TEMP%\wv2-extract'"
if errorlevel 1 exit /b 1

set "SRC=%TEMP%\wv2-extract"
if not exist "%ROOT%\third_party\webview2\include" mkdir "%ROOT%\third_party\webview2\include"
if not exist "%ROOT%\third_party\webview2\lib\x64" mkdir "%ROOT%\third_party\webview2\lib\x64"
copy /y "%SRC%\build\native\include\WebView2.h" "%ROOT%\third_party\webview2\include\" >nul
copy /y "%SRC%\build\native\include\WebView2EnvironmentOptions.h" "%ROOT%\third_party\webview2\include\" >nul
copy /y "%SRC%\build\native\x64\WebView2LoaderStatic.lib" "%ROOT%\third_party\webview2\lib\x64\" >nul
copy /y "%SRC%\LICENSE.txt" "%ROOT%\third_party\webview2\" >nul
copy /y "%SRC%\NOTICE.txt" "%ROOT%\third_party\webview2\" >nul

echo Extracted %VERSION% into third_party\webview2
echo Update VERSION if the version changed.
exit /b 0
