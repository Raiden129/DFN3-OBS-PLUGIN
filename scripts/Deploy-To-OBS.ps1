param(
    [string]$ObsRoot = "C:\Program Files\obs-studio",
    [string]$BuildConfig = "Release",
    [string]$PluginDllPath = "",
    [string]$DeepFilterDllPath = "",
    [string]$StandardModelPath = "",
    [switch]$SkipDeepFilter,
    [switch]$SkipModels,
    [switch]$NoAutoBootstrap
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path $PSScriptRoot -Parent
$bootstrapScript = Join-Path $PSScriptRoot "Bootstrap-DeepFilterAssets.ps1"
$bootstrapOutputDir = Join-Path $repoRoot "build\runtime-assets"

function Resolve-DefaultPaths {
    param()

    if (-not $DeepFilterDllPath) {
        $dllCandidates = @(
            (Join-Path $repoRoot "deepfilter.dll"),
            (Join-Path $repoRoot "data\obs-plugins\obs-dfn3-noise-suppress\deepfilter.dll"),
            (Join-Path $bootstrapOutputDir "deepfilter.dll")
        )
        $script:DeepFilterDllPath = $dllCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    }

    if (-not $StandardModelPath) {
        $stdCandidates = @(
            (Join-Path $repoRoot "data\models\DeepFilterNet3_onnx.tar.gz"),
            (Join-Path $bootstrapOutputDir "models\DeepFilterNet3_onnx.tar.gz")
        )
        $script:StandardModelPath = $stdCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    }

}

function Ensure-RuntimeAssets {
    param()

    Resolve-DefaultPaths

    $needDeepFilter = (-not $SkipDeepFilter) -and (-not $DeepFilterDllPath -or -not (Test-Path $DeepFilterDllPath))
    $needStandardModel = (-not $SkipModels) -and (-not $StandardModelPath -or -not (Test-Path $StandardModelPath))

    if (($needDeepFilter -or $needStandardModel) -and (-not $NoAutoBootstrap)) {
        if (-not (Test-Path $bootstrapScript)) {
            throw "Missing runtime bootstrap script: $bootstrapScript"
        }

        Write-Host "Runtime assets missing. Bootstrapping DeepFilter assets..." -ForegroundColor Cyan
        & $bootstrapScript -OutputDir $bootstrapOutputDir
        if ($LASTEXITCODE -ne 0) {
            throw "Asset bootstrap failed with exit code $LASTEXITCODE"
        }

        Resolve-DefaultPaths
    }

    if (-not $SkipDeepFilter) {
        if (-not $DeepFilterDllPath -or -not (Test-Path $DeepFilterDllPath)) {
            throw "deepfilter.dll not found. Set -DeepFilterDllPath or allow auto bootstrap."
        }
    }

    if (-not $SkipModels) {
        if (-not $StandardModelPath -or -not (Test-Path $StandardModelPath)) {
            throw "DeepFilterNet3_onnx.tar.gz not found. Set -StandardModelPath or allow auto bootstrap."
        }
    }
}

if (-not $PluginDllPath) {
    $PluginDllPath = Join-Path $repoRoot "build\$BuildConfig\obs-dfn3-noise-suppress.dll"
}

if (-not (Test-Path $PluginDllPath)) {
    throw "Plugin DLL not found: $PluginDllPath"
}

Ensure-RuntimeAssets

$pluginBinDir = Join-Path $ObsRoot "obs-plugins\64bit"
$pluginDataDir = Join-Path $ObsRoot "data\obs-plugins\obs-dfn3-noise-suppress"
$modelDstDir = Join-Path $pluginDataDir "models"

New-Item -ItemType Directory -Force $pluginBinDir, $pluginDataDir, $modelDstDir | Out-Null

$pluginDst = Join-Path $pluginBinDir "obs-dfn3-noise-suppress.dll"
Copy-Item $PluginDllPath $pluginDst -Force
Write-Host "Copied plugin DLL: $pluginDst"

if (-not $SkipDeepFilter) {
    if ($DeepFilterDllPath -and (Test-Path $DeepFilterDllPath)) {
        $dfDst = Join-Path $pluginDataDir "deepfilter.dll"
        Copy-Item $DeepFilterDllPath $dfDst -Force
        Write-Host "Copied DeepFilter runtime: $dfDst"
    } else {
        throw "deepfilter.dll not found after bootstrap."
    }
}

if (-not $SkipModels) {
    if ($StandardModelPath -and (Test-Path $StandardModelPath)) {
        $stdDst = Join-Path $modelDstDir "DeepFilterNet3_onnx.tar.gz"
        Copy-Item $StandardModelPath $stdDst -Force
        Write-Host "Copied standard model: $stdDst"
    } else {
        throw "Standard model not found after bootstrap."
    }
}

Write-Host "Deployment complete."
Write-Host "Plugin binary directory: $pluginBinDir"
Write-Host "Plugin data directory:   $pluginDataDir"
