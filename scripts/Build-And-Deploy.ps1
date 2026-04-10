param(
    [string]$ObsRoot = "C:\Program Files\obs-studio",
    [string]$ObsSdkRoot = "",
    [string]$BuildDir = "build",
    [string]$BuildConfig = "Release",
    [switch]$NoAutoBootstrap,
    [switch]$SkipDeepFilter,
    [switch]$SkipModels
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path $PSScriptRoot -Parent
$resolveScript = Join-Path $PSScriptRoot "Resolve-ObsSdk.ps1"
$deployScript = Join-Path $PSScriptRoot "Deploy-To-OBS.ps1"

if (-not (Test-Path $resolveScript)) {
    throw "Missing script: $resolveScript"
}

if (-not (Test-Path $deployScript)) {
    throw "Missing script: $deployScript"
}

Push-Location $repoRoot
try {
    Write-Host "Resolving OBS SDK and configuring CMake..." -ForegroundColor Cyan
    if ($ObsSdkRoot) {
        & $resolveScript -ObsRoot $ObsSdkRoot -Configure -BuildDir $BuildDir
    } else {
        & $resolveScript -Configure -BuildDir $BuildDir
    }
    if ($LASTEXITCODE -ne 0) {
        throw "OBS SDK resolution/configure failed."
    }

    Write-Host "Building plugin ($BuildConfig)..." -ForegroundColor Cyan
    & cmake --build $BuildDir --config $BuildConfig
    if ($LASTEXITCODE -ne 0) {
        throw "Plugin build failed."
    }

    Write-Host "Deploying plugin to OBS..." -ForegroundColor Cyan
    & $deployScript -ObsRoot $ObsRoot -BuildConfig $BuildConfig -NoAutoBootstrap:$NoAutoBootstrap -SkipDeepFilter:$SkipDeepFilter -SkipModels:$SkipModels
    if ($LASTEXITCODE -ne 0) {
        throw "Plugin deployment failed."
    }

    Write-Host "Build and deployment complete." -ForegroundColor Green
    Write-Host "Restart OBS if it was open before deployment."
} finally {
    Pop-Location
}
