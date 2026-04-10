param(
    [string]$ObsRoot = "",
    [switch]$Configure,
    [string]$BuildDir = "build",
    [string]$Generator = "Visual Studio 17 2022"
)

$script = Join-Path $PSScriptRoot "scripts\Resolve-ObsSdk.ps1"
if (-not (Test-Path $script)) {
    Write-Error "Missing helper script: $script"
    exit 1
}

& $script -ObsRoot $ObsRoot -Configure:$Configure -BuildDir $BuildDir -Generator $Generator
exit $LASTEXITCODE
