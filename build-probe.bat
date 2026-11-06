@echo off
rem Build the stage 0 probe plugin and make an MO2-installable zip.
rem The version and the zip path come from CMakeLists.txt (project VERSION, TN_BUILD_ROOT).
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if "%VCPKG_ROOT%"=="" set "VCPKG_ROOT=%USERPROFILE%\vcpkg"
cd /d "%~dp0"
cmake --preset vs2022-release || exit /b 1
cmake --build --preset vs2022-release --target package_probe || exit /b 1
