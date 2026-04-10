param(
    [string]$ObsRoot = "C:\Program Files\obs-studio",
    [string]$ObsSdkRoot = "",
    [string]$BuildDir = "build",
    [string]$BuildConfig = "Release",
    [switch]$NoAutoBootstrap,
    [switch]$SkipDeepFilter,
    [switch]$SkipModels
)

$script = Join-Path $PSScriptRoot "scripts\Build-And-Deploy.ps1"
if (-not (Test-Path $script)) {
    Write-Error "Missing helper script: $script"
    exit 1
}

& $script -ObsRoot $ObsRoot -ObsSdkRoot $ObsSdkRoot -BuildDir $BuildDir -BuildConfig $BuildConfig -NoAutoBootstrap:$NoAutoBootstrap -SkipDeepFilter:$SkipDeepFilter -SkipModels:$SkipModels
exit $LASTEXITCODE
