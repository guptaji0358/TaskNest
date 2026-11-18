@echo off
rem Compile src -> out\bin\TaskNest.exe (+TaskNestHost.exe). Add "test" to run ctest. Never touches out\bin\data.
setlocal
cd /d "%~dp0..\.."
set "QT=D:\Apps\Qt6"
set "PATH=%QT%\Tools\mingw1310_64\bin;%QT%\Tools\Ninja;%QT%\Tools\CMake_64\bin;%QT%\6.12.0\mingw_64\bin;%PATH%"
if not exist out\build\CMakeCache.txt (
  cmake -S . -B out/build -G Ninja -DCMAKE_PREFIX_PATH=D:/Apps/Qt6/6.12.0/mingw_64 -DCMAKE_CXX_COMPILER=D:/Apps/Qt6/Tools/mingw1310_64/bin/g++.exe || exit /b 1
)
cmake --build out/build || exit /b 1
if /i "%~1"=="test" ctest --test-dir out/build --output-on-failure
