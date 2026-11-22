@echo off
setlocal

set "VSCMD_SKIP_SENDTELEMETRY=1"
set "ROOT=%~dp0"
set "BUILD_DIR=%ROOT%build"
set "OBJ_DIR=%BUILD_DIR%\obj"

rem Localiza qualquer Visual Studio/Build Tools com o compilador x64 (Community, Professional, 2022, 18...).
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSINSTALL="
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
)
if not defined VSINSTALL (
    echo [ERRO] Visual Studio com MSVC x64 nao encontrado ^(vswhere^).
    exit /b 1
)
set "VCVARS=%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo [ERRO] vcvars64.bat nao encontrado em: "%VCVARS%"
    exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 (
    echo [ERRO] Falha ao inicializar ambiente MSVC x64.
    exit /b 1
)

if exist "%BUILD_DIR%\CMakeFiles" rd /s /q "%BUILD_DIR%\CMakeFiles"
if exist "%BUILD_DIR%\CMakeCache.txt" del /f /q "%BUILD_DIR%\CMakeCache.txt"
if not exist "%OBJ_DIR%" mkdir "%OBJ_DIR%"

if exist "%BUILD_DIR%\TaskbarPerf.old.exe" del /f /q "%BUILD_DIR%\TaskbarPerf.old.exe" >nul 2>&1
if exist "%BUILD_DIR%\TaskbarPerf.exe" (
    del /f /q "%BUILD_DIR%\TaskbarPerf.exe" >nul 2>&1
    if exist "%BUILD_DIR%\TaskbarPerf.exe" (
        move /y "%BUILD_DIR%\TaskbarPerf.exe" "%BUILD_DIR%\TaskbarPerf.old.exe" >nul 2>&1
    )
)

rem Hardening: /sdl (checagens extras), /guard:cf (CFG), /CETCOMPAT (shadow stack) e
rem /DEPENDENTLOADFLAG:0x800 (DLLs importadas so de System32, nunca da pasta do exe).
cl /nologo /std:c++20 /O2 /GL /MP /W4 /sdl /guard:cf /utf-8 /permissive- /EHsc ^
    /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 ^
    /Fo:"%OBJ_DIR%\\" ^
    /Fe:"%BUILD_DIR%\TaskbarPerf.exe" ^
    "%ROOT%src\config.cpp" ^
    "%ROOT%src\metrics_collector.cpp" ^
    "%ROOT%src\d2d_layered.cpp" ^
    "%ROOT%src\taskbar_tracker.cpp" ^
    "%ROOT%src\taskbar_bar.cpp" ^
    "%ROOT%src\flyout_window.cpp" ^
    "%ROOT%src\main.cpp" ^
    /link /SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF ^
    /GUARD:CF /CETCOMPAT /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /DEPENDENTLOADFLAG:0x800 ^
    user32.lib gdi32.lib dwmapi.lib d2d1.lib dwrite.lib dxgi.lib pdh.lib ^
    iphlpapi.lib shell32.lib shcore.lib advapi32.lib ole32.lib powrprof.lib wtsapi32.lib
if errorlevel 1 (
    echo [ERRO] Falha na compilacao.
    exit /b 1
)

mt.exe -nologo -manifest "%ROOT%resources\app.manifest" -outputresource:"%BUILD_DIR%\TaskbarPerf.exe;1"
if errorlevel 1 (
    echo [ERRO] Falha ao embutir o manifesto.
    exit /b 1
)

set "EXE=%BUILD_DIR%\TaskbarPerf.exe"
if not exist "%EXE%" (
    echo [ERRO] Executavel nao encontrado em "%EXE%".
    exit /b 1
)

for %%F in ("%EXE%") do (
    echo [OK] Compilado: %%~fF ^(%%~zF bytes^)
)
endlocal
