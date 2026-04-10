param(
    [string]$ObsRoot = "",
    [switch]$Configure,
    [string]$BuildDir = "build",
    [string]$Generator = "Visual Studio 17 2022"
)

$ErrorActionPreference = "Stop"

function Find-FirstFile {
    param(
        [string[]]$Paths,
        [string[]]$Names
    )

    foreach ($p in $Paths) {
        if (-not (Test-Path $p)) {
            continue
        }

        foreach ($n in $Names) {
            $match = Get-ChildItem -Path $p -Recurse -Filter $n -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($null -ne $match) {
                return $match.FullName
            }
        }
    }

    return $null
}

function Get-CandidateRoots {
    param([string]$UserRoot)

    $candidates = New-Object System.Collections.Generic.List[string]

    if ($UserRoot) {
        $candidates.Add($UserRoot)
    }

    if ($env:OBS_ROOT_DIR) {
        $candidates.Add($env:OBS_ROOT_DIR)
    }

        $userHome = $env:USERPROFILE
    $defaults = @(
        "C:\dev\obs-sdk",
        "C:\dev\obs-studio",
            "$userHome\source\repos\obs-studio",
            "$userHome\source\obs-studio",
            "$userHome\obs-studio",
        "C:\Program Files\obs-studio"
    )

    foreach ($d in $defaults) {
        $candidates.Add($d)
    }

    return ($candidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -Unique)
}

function Resolve-ObsSdk {
    param([string[]]$Roots)

    foreach ($root in $Roots) {
        $probePaths = @(
            $root,
            (Join-Path $root "include"),
            (Join-Path $root "libobs"),
            (Join-Path $root "obs-studio"),
            (Join-Path $root "build"),
            (Join-Path $root "build\libobs\RelWithDebInfo"),
            (Join-Path $root "build\rundir\RelWithDebInfo\bin\64bit"),
            (Join-Path $root "lib")
        )

        $header = Find-FirstFile -Paths $probePaths -Names @("obs-module.h")
        $importLib = Find-FirstFile -Paths $probePaths -Names @("obs.lib", "libobs.lib")
        $pkgConfig = Find-FirstFile -Paths $probePaths -Names @("libobsConfig.cmake", "libobs-config.cmake")

        if ($header -and ($importLib -or $pkgConfig)) {
            return [PSCustomObject]@{
                Root = $root
                Header = $header
                ImportLib = $importLib
                PackageConfig = $pkgConfig
            }
        }
    }

    return $null
}

$roots = Get-CandidateRoots -UserRoot $ObsRoot

Write-Host "Scanning OBS SDK candidates:" -ForegroundColor Cyan
$roots | ForEach-Object { Write-Host "  $_" }

$resolved = Resolve-ObsSdk -Roots $roots

if (-not $resolved) {
    Write-Host "";
    Write-Host "No valid OBS SDK root was found." -ForegroundColor Yellow
    Write-Host "Need at minimum: obs-module.h and obs.lib/libobs.lib (or libobsConfig.cmake)." -ForegroundColor Yellow
    Write-Host "See docs/OBS_WINDOWS_SETUP.md for exact setup steps." -ForegroundColor Yellow
    exit 1
}

Write-Host "";
Write-Host "Resolved OBS SDK:" -ForegroundColor Green
Write-Host "  Root:        $($resolved.Root)"
Write-Host "  Header:      $($resolved.Header)"
Write-Host "  Import Lib:  $($resolved.ImportLib)"
Write-Host "  CMake Config:$($resolved.PackageConfig)"

$cmd = "cmake -S . -B $BuildDir -DOBS_ROOT_DIR=`"$($resolved.Root)`""
if ($resolved.PackageConfig) {
    $cmd += " -DCMAKE_PREFIX_PATH=`"$($resolved.Root)`""
}

Write-Host "";
Write-Host "Suggested configure command:" -ForegroundColor Cyan
Write-Host "  $cmd"

if ($Configure) {
    Write-Host "";
    Write-Host "Running configure..." -ForegroundColor Cyan
    Invoke-Expression $cmd
}
