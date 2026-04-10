param(
    [string]$OutputDir = "",
    [string]$ModelUrl = "https://github.com/Rikorose/DeepFilterNet/raw/main/models/DeepFilterNet3_onnx.tar.gz",
    [string]$DeepFilterRepoDir = "",
    [switch]$SkipModelDownload,
    [switch]$SkipRuntimeBuild
)

$ErrorActionPreference = "Stop"

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$File,
        [string[]]$CommandArgs = @(),
        [string]$ErrorMessage = "Command failed."
    )

    & $File @CommandArgs
    if ($LASTEXITCODE -ne 0) {
        throw "$ErrorMessage Exit code: $LASTEXITCODE"
    }
}

function Copy-FirstExistingItem {
    param(
        [string[]]$Candidates,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            Copy-Item $candidate $Destination -Force
            return $true
        }
    }

    return $false
}

function Get-OrDownloadModelArchive {
    param(
        [Parameter(Mandatory = $true)][string]$DestinationPath,
        [Parameter(Mandatory = $true)][string]$DownloadUrl,
        [switch]$SkipDownload
    )

    if (Test-Path $DestinationPath) {
        Write-Host "Model archive already present: $DestinationPath"
        return
    }

    if ($SkipDownload) {
        throw "Model archive missing and -SkipModelDownload was provided: $DestinationPath"
    }

    Write-Host "Downloading standard DFN3 model archive..." -ForegroundColor Cyan
    Invoke-WebRequest -Uri $DownloadUrl -OutFile $DestinationPath

    if (-not (Test-Path $DestinationPath)) {
        throw "Model download did not produce expected file: $DestinationPath"
    }

    Write-Host "Downloaded model archive: $DestinationPath"
}

function Test-CargoSubcommand {
    param([Parameter(Mandatory = $true)][string]$Subcommand)

    try {
        & cargo $Subcommand --help *> $null
        return ($LASTEXITCODE -eq 0)
    } catch {
        return $false
    }
}

function Get-OrBuildDeepFilterDll {
    param(
        [Parameter(Mandatory = $true)][string]$RepoRoot,
        [Parameter(Mandatory = $true)][string]$OutputDllPath,
        [string]$RepoDir,
        [switch]$SkipBuild
    )

    if (Test-Path $OutputDllPath) {
        Write-Host "DeepFilter runtime already present: $OutputDllPath"
        return
    }

    $localCandidates = @(
        (Join-Path $RepoRoot "deepfilter.dll"),
        (Join-Path $RepoRoot "data\obs-plugins\obs-dfn3-noise-suppress\deepfilter.dll")
    )

    if (Copy-FirstExistingItem -Candidates $localCandidates -Destination $OutputDllPath) {
        Write-Host "Copied existing DeepFilter runtime: $OutputDllPath"
        return
    }

    if ($SkipBuild) {
        throw "deepfilter.dll not found and -SkipRuntimeBuild was provided."
    }

    $gitCmd = Get-Command git -ErrorAction SilentlyContinue
    if (-not $gitCmd) {
        throw "git is required to build deepfilter.dll automatically."
    }

    $cargoCmd = Get-Command cargo -ErrorAction SilentlyContinue
    if (-not $cargoCmd) {
        throw "Rust cargo is required to build deepfilter.dll automatically. Install Rust first."
    }

    if (-not (Test-CargoSubcommand -Subcommand "cinstall")) {
        Write-Host "Installing cargo-c (required for cargo cinstall)..." -ForegroundColor Cyan
        Invoke-Checked -File "cargo" -CommandArgs @("install", "cargo-c", "--locked") -ErrorMessage "Failed to install cargo-c."
    }

    if (-not $RepoDir) {
        $RepoDir = Join-Path $RepoRoot "build\_deps\DeepFilterNet"
    }

    if (-not (Test-Path $RepoDir)) {
        $repoParent = Split-Path $RepoDir -Parent
        New-Item -ItemType Directory -Force $repoParent | Out-Null
        Write-Host "Cloning DeepFilterNet repository..." -ForegroundColor Cyan
        Invoke-Checked -File "git" -CommandArgs @("clone", "--depth", "1", "https://github.com/Rikorose/DeepFilterNet.git", $RepoDir) -ErrorMessage "Failed to clone DeepFilterNet repository."
    }

    $stageDir = Join-Path $RepoRoot "build\runtime-assets\deepfilter-stage"
    if (Test-Path $stageDir) {
        Remove-Item -Recurse -Force $stageDir
    }
    New-Item -ItemType Directory -Force $stageDir | Out-Null

    Write-Host "Building deepfilter.dll from DeepFilterNet (this may take a while)..." -ForegroundColor Cyan
    Push-Location $RepoDir
    try {
        Invoke-Checked -File "cargo" -CommandArgs @("cinstall", "--profile=release-lto", "-p", "deep_filter", "--destdir", $stageDir, "--target", "x86_64-pc-windows-msvc") -ErrorMessage "Failed to build/install deepfilter runtime."
    } finally {
        Pop-Location
    }

    $dll = Get-ChildItem -Path $stageDir -Recurse -Include "deepfilter.dll", "deep_filter.dll" -File -ErrorAction SilentlyContinue |
        Select-Object -First 1

    if (-not $dll) {
        throw "Built runtime staging did not contain deepfilter.dll."
    }

    Copy-Item $dll.FullName $OutputDllPath -Force
    Write-Host "Built and staged DeepFilter runtime: $OutputDllPath"
}

$repoRoot = Split-Path $PSScriptRoot -Parent

if (-not $OutputDir) {
    $OutputDir = Join-Path $repoRoot "build\runtime-assets"
}

$modelDir = Join-Path $OutputDir "models"
$modelPath = Join-Path $modelDir "DeepFilterNet3_onnx.tar.gz"
$runtimeDllPath = Join-Path $OutputDir "deepfilter.dll"

New-Item -ItemType Directory -Force $OutputDir, $modelDir | Out-Null

Get-OrDownloadModelArchive -DestinationPath $modelPath -DownloadUrl $ModelUrl -SkipDownload:$SkipModelDownload
Get-OrBuildDeepFilterDll -RepoRoot $repoRoot -OutputDllPath $runtimeDllPath -RepoDir $DeepFilterRepoDir -SkipBuild:$SkipRuntimeBuild

Write-Host "DeepFilter runtime assets ready." -ForegroundColor Green
Write-Host "  Model:   $modelPath"
Write-Host "  Runtime: $runtimeDllPath"
