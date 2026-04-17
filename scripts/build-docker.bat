@echo off
setlocal

set "ROOT=%~dp0.."
set "IMAGE=devkitpro/devkita64"

where docker >nul 2>nul
if errorlevel 1 (
    echo Docker is not installed or is not available in PATH.
    exit /b 1
)

docker version >nul 2>nul
if errorlevel 1 (
    echo Docker is installed but is not responding. Start Docker Desktop and try again.
    exit /b 1
)

docker image inspect %IMAGE% >nul 2>nul
if errorlevel 1 (
    echo Docker image '%IMAGE%' not found. Pulling...
    docker pull %IMAGE%
    if errorlevel 1 (
        echo Failed to pull Docker image '%IMAGE%'.
        exit /b 1
    )
) else (
    echo Docker image '%IMAGE%' already available.
)

docker run --rm -v "%ROOT%:/workspace" -w /workspace/sysmodule %IMAGE% /bin/bash -lc "make install-layout"
if errorlevel 1 (
    echo Build command failed: make install-layout
    exit /b 1
)

if not exist "%ROOT%\romfs\sysmodule" mkdir "%ROOT%\romfs\sysmodule"
copy /Y "%ROOT%\sysmodule\atmosphere\contents\0100000000000F12\exefs.nsp" "%ROOT%\romfs\sysmodule\exefs.nsp" >nul
if errorlevel 1 (
    echo Failed to stage sysmodule exefs.nsp into romfs.
    exit /b 1
)
type nul > "%ROOT%\romfs\sysmodule\boot2.flag"

if exist "%ROOT%\build\sysmodule_blob.o" del "%ROOT%\build\sysmodule_blob.o"
if exist "%ROOT%\switch-ha.elf" del "%ROOT%\switch-ha.elf"
if exist "%ROOT%\switch-ha.nro" del "%ROOT%\switch-ha.nro"

docker run --rm -v "%ROOT%:/workspace" -w /workspace %IMAGE% /bin/bash -lc "make"
if errorlevel 1 (
    echo Build command failed: make
    exit /b 1
)

echo Build completed.
exit /b 0
