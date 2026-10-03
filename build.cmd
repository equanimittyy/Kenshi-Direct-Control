@echo off
setlocal
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
echo.
echo Mod folder: dist\WASDCombatPlugin\
:end
echo.
echo Press any key to close.
pause >nul
