@echo off
setlocal enabledelayedexpansion
:: build.bat — Download cosmocc + cosmos, compile webview_shim + main.c into APE
::
:: cosmocc is a bourne shell script on Windows, so we need cosmos' bash.exe.
:: cosmos provides native Windows bash + coreutils — no WSL, Cygwin, or MSYS.
::
:: Usage: build.bat
:: Output: .\webview_demo.com

set COSMOCC_URL=https://cosmo.zip/pub/cosmocc/cosmocc.zip
set COSMOS_URL=https://cosmo.zip/pub/cosmos/zip/cosmos.zip
set OUTPUT=webview_demo.com

if defined LOCALAPPDATA (set "CACHE_DIR=%LOCALAPPDATA%\cosmocc") else (set "CACHE_DIR=%USERPROFILE%\.cache\cosmocc")

set "BASH=%CACHE_DIR%\bin\bash.exe"
set "COSMOCC_DIR=%CACHE_DIR%\cosmocc\bin"
set "ZIP_COSMOCC=%CACHE_DIR%\cosmocc-latest.zip"
set "ZIP_COSMOS=%CACHE_DIR%\cosmos.zip"

if not exist "%CACHE_DIR%" mkdir "%CACHE_DIR%"

echo ==^> Checking for cosmocc + cosmos in %CACHE_DIR%...
if not exist "%BASH%" (
    echo     Not found. Downloading cosmos...
    powershell -NoProfile -Command "[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; Invoke-WebRequest -Uri '%COSMOS_URL%' -OutFile '%ZIP_COSMOS%' -UseBasicParsing"
    if !ERRORLEVEL! NEQ 0 (echo ERROR: Download failed.& exit /b 1)

    echo     Extracting cosmos...
    powershell -NoProfile -Command "$t='%CACHE_DIR%\tmp_cosmos'; Expand-Archive -Path '%ZIP_COSMOS%' -DestinationPath $t -Force; $c=Get-ChildItem $t; if($c.Count -eq 1 -and $c[0].PSIsContainer){Get-ChildItem $c[0].FullName|Move-Item -Destination '%CACHE_DIR%' -Force}else{Get-ChildItem $t|Move-Item -Destination '%CACHE_DIR%' -Force}; Remove-Item $t -Recurse"
    if !ERRORLEVEL! NEQ 0 (echo ERROR: Extraction failed.& exit /b 1)

    if exist "%CACHE_DIR%\bin\bash" if not exist "%CACHE_DIR%\bin\bash.exe" (
        copy /y "%CACHE_DIR%\bin\bash" "%CACHE_DIR%\bin\bash.exe" >nul
    )
    del /f /q "%ZIP_COSMOS%">nul 2>&1
    echo     Done.
)

if not exist "%COSMOCC_DIR%\cosmocc" (
    echo     Not found. Downloading cosmocc...
    if exist "%CACHE_DIR%\cosmocc" rmdir /s /q "%CACHE_DIR%\cosmocc"

    powershell -NoProfile -Command "[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; Invoke-WebRequest -Uri '%COSMOCC_URL%' -OutFile '%ZIP_COSMOCC%' -UseBasicParsing"
    if !ERRORLEVEL! NEQ 0 (echo ERROR: Download failed.& exit /b 1)

    echo     Extracting cosmocc...
    if exist "%CACHE_DIR%\cosmocc" rmdir /s /q "%CACHE_DIR%\cosmocc"
    powershell -NoProfile -Command "Expand-Archive -Path '%ZIP_COSMOCC%' -DestinationPath '%CACHE_DIR%\cosmocc' -Force; $c=Get-ChildItem '%CACHE_DIR%\cosmocc'; if($c.Count -eq 1 -and $c[0].PSIsContainer){$d=$c[0].FullName; Get-ChildItem $d|Move-Item -Destination '%CACHE_DIR%\cosmocc' -Force; Remove-Item $d -Recurse}"
    if !ERRORLEVEL! NEQ 0 (echo ERROR: Extraction failed.& exit /b 1)

    del /f /q "%ZIP_COSMOCC%">nul 2>&1
    echo     Done.
) else (echo     Found cached cosmocc.)

:: Verify sources
for %%f in (main.c webview_shim.c webview_shim.h) do (
    if not exist "%%f" (echo ERROR: %%f not found.& exit /b 1)
)

echo ==^> Compiling %OUTPUT% with cosmocc via cosmos bash...
set "PATH=%COSMOCC_DIR%;%CACHE_DIR%\bin;%PATH%"

"%BASH%" "%COSMOCC_DIR%\cosmocc" -O2 -o "%OUTPUT%" main.c webview_shim.c 2>&1 || ver>nul

if exist "%OUTPUT%" (
    echo.
	echo ===============================================================
    echo   SUCCESS: %OUTPUT% built
    echo   This APE auto-detects the OS at runtime.
    echo   Run it: %OUTPUT%
    echo ===============================================================
) else (echo ERROR: Binary not found.& exit /b 1)
endlocal
