param(
    [string]$FarSource = "",
    [string]$Generator = "Visual Studio 17 2022"
)

if (-not $FarSource) {
    $FarSource = Read-Host "Path to Far Manager source tree"
}

cmake -S . -B build -G $Generator -A x64 "-DFAR_SOURCE_DIR=$FarSource"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build build --config Release
exit $LASTEXITCODE
