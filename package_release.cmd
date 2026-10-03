@echo off
setlocal
:menu
echo Select one:
echo 1. Rebuild and repackage
echo 2. Rebuild only
echo 3. Repackage
echo 4. Exit
set "OPTION="
set /p "OPTION=> "
if "%OPTION%"=="4" exit /b
if "%OPTION%"=="3" goto package
if not "%OPTION%"=="1" if not "%OPTION%"=="2" (
    echo Invalid selection.
    echo.
    goto menu
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD=%%i"
if not defined MSBUILD (
    echo MSBuild not found. See docs\plugin_build_setup.md, section 2.
    goto end
)
"%MSBUILD%" "%~dp0WASDCombatPlugin.sln" -p:Configuration=Release -p:Platform=x64 -nologo -verbosity:minimal
if errorlevel 1 (
    echo.
    echo Build failed.
    goto end
)
if %OPTION%==2 goto end

:package
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\package_release.ps1"
if errorlevel 1 (
    echo.
    echo Packaging failed.
)
:end
echo.
echo Press any key to close.
pause >nul
