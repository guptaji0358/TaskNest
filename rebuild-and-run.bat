@echo off
rem Stop the running TaskNest (this checkout only), rebuild, launch.
rem Touches out\build and out\bin binaries only - never out\bin\data (your tasks).
setlocal
cd /d "%~dp0"

set "QT=D:\Apps\Qt6"
set "PATH=%QT%\Tools\mingw1310_64\bin;%QT%\Tools\Ninja;%QT%\Tools\CMake_64\bin;%QT%\6.11.1\mingw_64\bin;%PATH%"
set "BIN=%~dp0out\bin"

echo [1/3] Stopping TaskNest if running...
powershell -NoProfile -Command ^
  "$bin = [IO.Path]::GetFullPath('%BIN%');" ^
  "$p = Get-CimInstance Win32_Process -Filter \"Name='TaskNest.exe' OR Name='TaskNestHost.exe'\" | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($bin, 'OrdinalIgnoreCase') };" ^
  "foreach ($x in $p) { Write-Host ('  stopping ' + $x.Name + ' (pid ' + $x.ProcessId + ')'); Stop-Process -Id $x.ProcessId -Force -ErrorAction SilentlyContinue };" ^
  "Start-Sleep -Milliseconds 500"

echo [2/3] Building...
if not exist "out\build\CMakeCache.txt" (
    cmake -S . -B out/build -G Ninja -DCMAKE_PREFIX_PATH=D:/Apps/Qt6/6.11.1/mingw_64 -DCMAKE_CXX_COMPILER=D:/Apps/Qt6/Tools/mingw1310_64/bin/g++.exe
    if errorlevel 1 goto :fail
)
cmake --build out/build
if errorlevel 1 goto :fail

echo [3/3] Launching...
start "" "%BIN%\TaskNest.exe"
exit /b 0

:fail
echo.
echo Build failed - TaskNest was not launched.
pause
exit /b 1
