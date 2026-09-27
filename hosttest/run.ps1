# Build and run the host-side decision tests on Windows.
# Finds whatever C++ compiler you have; no ESP32 and no PlatformIO needed.
#
#   0 = all good   1 = regression   2 = known bugs present   3 = no compiler

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

# main.cpp does #include "config.h". That resolves to ..\src\config.h when it
# exists; on a fresh clone it will not (git-ignored, correctly), so make a
# dummy. Never contains real credentials.
if (-not (Test-Path ..\src\config.h) -and -not (Test-Path stubs\config.h)) {
@'
#pragma once
#define WIFI_SSID "hosttest"
#define WIFI_PASSWORD "hosttest"
#define MQTT_SERVER "127.0.0.1"
#define MQTT_PORT 1883
#define MQTT_USER "hosttest"
#define MQTT_PASSWORD "hosttest"
'@ | Set-Content stubs\config.h -Encoding ASCII
  Write-Host "(generated stubs\config.h - no real credentials involved)"
}

$exe = Join-Path $env:TEMP 'flushwater_tests.exe'
if (Test-Path $exe) { Remove-Item $exe -Force }

function Have($name) { $null -ne (Get-Command $name -ErrorAction SilentlyContinue) }

if (Have g++) {
  Write-Host "compiler: g++"
  & g++ -std=gnu++17 -O0 -g -I stubs -o $exe test_decide.cpp
}
elseif (Have clang++) {
  Write-Host "compiler: clang++"
  & clang++ -std=gnu++17 -O0 -g -I stubs -o $exe test_decide.cpp
}
elseif (Have wsl) {
  Write-Host "compiler: g++ inside WSL"
  & wsl bash ./run.sh
  exit $LASTEXITCODE
}
elseif (Have cl) {
  # /std:c++20 because main.cpp uses designated initialisers in wdtSetup().
  Write-Host "compiler: MSVC cl"
  & cl /nologo /std:c++20 /EHsc /I stubs test_decide.cpp /Fe:$exe | Out-Null
}
else {
  Write-Host ""
  Write-Host "No C++ compiler found. Pick whichever is least annoying:" -ForegroundColor Yellow
  Write-Host "  winget install -e --id MSYS2.MSYS2      then: pacman -S mingw-w64-ucrt-x86_64-gcc"
  Write-Host "  winget install -e --id LLVM.LLVM        (clang++)"
  Write-Host "  wsl --install                           (then this script uses WSL automatically)"
  Write-Host ""
  Write-Host "Or just ask Claude to run them - the harness works in its Linux sandbox."
  exit 3
}

if (-not (Test-Path $exe)) { Write-Host "BUILD FAILED" -ForegroundColor Red; exit 3 }
& $exe
exit $LASTEXITCODE
