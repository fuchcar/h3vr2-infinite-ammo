param([string]$BuildType = "RelWithDebInfo")

# Resolve cmake/ninja from PATH, falling back to a local Tools install.
function Find-Tool($name, $fallback) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    if (Test-Path $fallback) { return $fallback }
    throw "$name not found on PATH. Install it or edit the fallback path in build.ps1."
}
$cmake = Find-Tool "cmake" "$env:USERPROFILE\Tools\cmake\cmake-4.5.0-windows-x86_64\bin\cmake.exe"
$ninja = Find-Tool "ninja" "$env:USERPROFILE\Tools\ninja\ninja.exe"

& $cmake -G "Ninja" -DCMAKE_MAKE_PROGRAM="$ninja" -DCMAKE_BUILD_TYPE="$BuildType" . -B build
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }

& $cmake --build ./build
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }

Write-Host "Built build/libh3vr2infammo.so"
