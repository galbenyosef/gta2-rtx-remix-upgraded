@echo off
setlocal
rem One click: build the renderer, copy it into the game folder, start the game.
rem Keep this file next to the "dll" folder, e.g. G:\gta2-rtx-remix-carbodies-src\update.bat
set "GAME=G:\Grand Theft Auto 2"
set "SRC=%~dp0"

tasklist /fi "imagename eq gta2.exe" 2>nul | find /i "gta2.exe" >nul
if not errorlevel 1 (
    echo GTA2 is still running - close it first, then run this again.
    pause
    exit /b 1
)

echo Building...
call "%SRC%dll\build.bat"
if errorlevel 1 (
    echo.
    echo BUILD FAILED - scroll up for the first error and send it to Claude.
    pause
    exit /b 1
)

if not exist "%SRC%dll\build\gta2dx9.dll" (
    echo Build did not produce gta2dx9.dll.
    pause
    exit /b 1
)

echo Installing into %GAME% ...
copy /y "%SRC%dll\build\gta2dx9.dll" "%GAME%\gta2dx9.dll" >nul || goto :fail
copy /y "%SRC%dll\build\gta2dx9.dll" "%GAME%\d3ddll.dll"   >nul || goto :fail

echo Starting GTA2...
start "" /d "%GAME%" "%GAME%\gta2.exe"
exit /b 0

:fail
echo Could not copy the DLL into the game folder.
pause
exit /b 1
