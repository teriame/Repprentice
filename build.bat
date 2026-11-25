@echo off
rem Build The Apprentice CD-i static recompilation on Windows as a single
rem self-contained executable in dist\Apprentice-<version>-x64.exe.
rem
rem Everything written during the build stays strictly inside CWD (build\,
rem gen\, dist\, saves\) or %TEMP%.  No loose apprentice.exe / apprentice_hd.exe
rem binaries are left in the root directory.
setlocal
cd /d "%~dp0"

set STUB_MODE=0
set COMPILE_ONLY=0
for %%a in (%*) do (
    if "%%a"=="--stub" set STUB_MODE=1
    if "%%a"=="--compile-only" set COMPILE_ONLY=1
)

for /f "delims=" %%n in ('python -c "import json;print(json.load(open('game.json')).get('exe_name','cdigame'))"') do set EXE=%%n
if "%EXE%"=="" goto :error
for /f "delims=" %%v in (VERSION) do set VER=%%v

if "%STUB_MODE%"=="1" (
    echo [1/3] Generating zero-derived-code stub tables ^(code will be derived at the user^)...
    python tools\gen.py --stub || goto :error
) else (
    if exist "rom\*.bin" (
        echo [1/3] Deriving 68000 code to C from your disc image...
        python tools\gen.py || goto :error
    ) else if exist "saves\*.bin" (
        echo [1/3] Deriving 68000 code to C from your disc image...
        python tools\gen.py || goto :error
    ) else if exist "rom\*.chd" (
        if not exist "saves" mkdir saves
        for %%c in ("rom\*.chd") do (
            echo Converting %%c to saves\%%~nc.bin/.cue...
            python tools\chd.py "%%c" "saves\%%~nc" || goto :error
        )
        echo [1/3] Deriving 68000 code to C from your disc image...
        python tools\gen.py || goto :error
    ) else (
        echo [1/3] No disc image in rom\ yet; building with zero-derived-code stub ^(derives at first run^)...
        python tools\gen.py --stub || goto :error
    )
)

echo [2/3] Configuring...
python tools\embed_tools.py build\cmake\embedded_tools.h || goto :error
cmake -S . -B build\cmake -G "Visual Studio 18 2026" -A x64 > nul || goto :error

echo [3/3] Compiling single self-contained executable...
cmake --build build\cmake --config Release --target game --parallel || goto :error

if exist "%EXE%.exe" del /q "%EXE%.exe"
if exist "%EXE%_hd.exe" del /q "%EXE%_hd.exe"

if "%COMPILE_ONLY%"=="0" (
    if not exist "dist" mkdir dist
    copy /y "build\cmake\Release\%EXE%.exe" "dist\Apprentice-%VER%-x64.exe" > nul || goto :error
    echo.
    echo Done: dist\Apprentice-%VER%-x64.exe ^(single self-contained executable^)
)
exit /b 0

:error
echo Build failed.
exit /b 1
