@echo off
rem Iki programi derler: Recording Studio.exe ve Game Tools.exe bir ust klasore gelir.
chcp 65001 >nul
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
set CIKTI=%TEMP%\recording-studio-build
if not exist "%CIKTI%" mkdir "%CIKTI%"
del /q "%CIKTI%\*.obj" 2>nul
rc /nologo /fo "%CIKTI%\studio.res" studio.rc
if errorlevel 1 goto hata
rc /nologo /fo "%CIKTI%\game_tools.res" game_tools.rc
if errorlevel 1 goto hata
rc /nologo /fo "%CIKTI%\editor.res" editor.rc
if errorlevel 1 goto hata
cl /nologo /c /O2 /MT /MP /EHsc /std:c++20 /utf-8 /W3 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /Iimgui /Fo"%CIKTI%\\" studio.cpp engine.cpp browser.cpp recorder.cpp encoder.cpp editor_core.cpp editor.cpp capture.cpp stream.cpp game_tools.cpp now_playing.cpp fps_meter.cpp graphics.cpp common.cpp language.cpp splash.cpp ui.cpp help.cpp imgui\imgui.cpp imgui\imgui_draw.cpp imgui\imgui_tables.cpp imgui\imgui_widgets.cpp imgui\imgui_impl_dx11.cpp imgui\imgui_impl_win32.cpp
if errorlevel 1 goto hata
set O=%CIKTI%
set ORTAK="%O%\graphics.obj" "%O%\common.obj" "%O%\language.obj" "%O%\splash.obj" "%O%\ui.obj" "%O%\help.obj" "%O%\imgui.obj" "%O%\imgui_draw.obj" "%O%\imgui_tables.obj" "%O%\imgui_widgets.obj" "%O%\imgui_impl_dx11.obj" "%O%\imgui_impl_win32.obj"
link /nologo /SUBSYSTEM:WINDOWS /OUT:"..\Recording Studio.exe" "%O%\studio.obj" "%O%\engine.obj" "%O%\browser.obj" webview2\WebView2LoaderStatic.lib CoreMessaging.lib version.lib mfsensorgroup.lib "%O%\recorder.obj" "%O%\encoder.obj" "%O%\capture.obj" "%O%\stream.obj" "%O%\fps_meter.obj" %ORTAK% "%O%\studio.res" iphlpapi.lib mmdevapi.lib pdh.lib d3d11.lib dxgi.lib d3dcompiler.lib d2d1.lib dwrite.lib windowscodecs.lib mfplat.lib mfreadwrite.lib mf.lib mfuuid.lib ole32.lib oleaut32.lib propsys.lib shell32.lib user32.lib gdi32.lib winmm.lib dwmapi.lib ws2_32.lib secur32.lib crypt32.lib advapi32.lib windowsapp.lib
if errorlevel 1 goto hata
link /nologo /SUBSYSTEM:WINDOWS /OUT:"..\Game Tools.exe" "%O%\game_tools.obj" "%O%\now_playing.obj" "%O%\fps_meter.obj" %ORTAK% "%O%\game_tools.res" windowsapp.lib ws2_32.lib iphlpapi.lib powrprof.lib d3d11.lib dxgi.lib d3dcompiler.lib d2d1.lib dwrite.lib windowscodecs.lib ole32.lib oleaut32.lib shell32.lib user32.lib gdi32.lib dwmapi.lib advapi32.lib
if errorlevel 1 goto hata
link /nologo /SUBSYSTEM:WINDOWS /OUT:"..\Video Editor.exe" "%O%\editor.obj" "%O%\editor_core.obj" "%O%\encoder.obj" %ORTAK% "%O%\editor.res" d3d11.lib dxgi.lib d3dcompiler.lib d2d1.lib dwrite.lib windowscodecs.lib ole32.lib oleaut32.lib shell32.lib user32.lib gdi32.lib dwmapi.lib advapi32.lib
if errorlevel 1 goto hata
rem Sanal kamera surucusu: Windows kamera hizmetinde calisir
cl /nologo /LD /O2 /MT /EHsc /std:c++20 /utf-8 /W3 /DUNICODE /D_UNICODE /Fo"%O%\\" /Fe"..\vcam.dll" vcam.cpp /link /DEF:vcam.def /IMPLIB:"%O%\vcam.lib" mfplat.lib mfuuid.lib advapi32.lib ole32.lib
if errorlevel 1 goto hata
rem Hizli derleme (build.bat hizli): yalnizca programlar; ceviri, paket ve kurulum atlanir
if /i "%~1"=="hizli" goto hizli
xcopy /y /q /i icons "..\icons" >nul
xcopy /y /q /i lib "..\lib" >nul
%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe /nologo /target:exe /platform:x64 /optimize+ /out:"..\lib\sensor.exe" /r:"..\lib\LibreHardwareMonitorLib.dll" /r:netstandard.dll /r:System.Management.dll sensor.cs
if errorlevel 1 goto hata
xcopy /y /q /i licenses "..\licenses" >nul
xcopy /y /q /i languages "..\languages" >nul
rem Kurulum dosyasi: kisisel ayarlar pakete girmez, kayitli crosshair profilleri girer
copy /y "..\crosshair-profiles.json" "..\preset-profiles.json" >nul
if errorlevel 1 goto hata
cl /nologo /O2 /MT /EHsc /std:c++20 /utf-8 /W3 /DUNICODE /D_UNICODE /Fo"%O%\\" /Fe"%O%\pack.exe" pack.cpp cabinet.lib
if errorlevel 1 goto hata
"%O%\pack.exe" "%O%\package.bin" .. "Recording Studio.exe" "Game Tools.exe" "Video Editor.exe" vcam.dll preset-profiles.json icons lib licenses languages
if errorlevel 1 goto hata
rc /nologo /i "%O%" /fo "%O%\setup.res" setup.rc
if errorlevel 1 goto hata
cl /nologo /c /O2 /MT /EHsc /std:c++20 /utf-8 /W3 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /Iimgui /Fo"%O%\\" setup.cpp
if errorlevel 1 goto hata
if not exist "..\Setup" mkdir "..\Setup"
link /nologo /SUBSYSTEM:WINDOWS /OUT:"..\Setup\Creator Suite Setup.exe" "%O%\setup.obj" %ORTAK% "%O%\setup.res" d3d11.lib dxgi.lib d3dcompiler.lib d2d1.lib dwrite.lib windowscodecs.lib cabinet.lib ole32.lib oleaut32.lib shell32.lib user32.lib gdi32.lib dwmapi.lib advapi32.lib
if errorlevel 1 goto hata
echo Derleme tamam: Recording Studio.exe, Game Tools.exe, Video Editor.exe ve Kurulum
exit /b 0
exit /b 0
:hizli
echo Hizli derleme tamam: Recording Studio.exe, Game Tools.exe, Video Editor.exe
exit /b 0
:hata
echo Derleme basarisiz.
exit /b 1