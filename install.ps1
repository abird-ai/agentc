# agentc installer for Windows: fetch a GitHub release archive, verify its
# SHA-256 and install agentc.exe. No build toolchain required.
#
#   irm https://raw.githubusercontent.com/abird-ai/agentc/master/install.ps1 | iex
#
# Parameters / environment overrides:
#   -Version        / AGENTC_VERSION           release to install (default "latest")
#   -InstallDir     / AGENTC_INSTALL_DIR      target directory (default "$HOME\.local\bin")
#   AGENTC_REPO                               GitHub owner/repo (default "abird-ai/agentc")
#   AGENTC_RELEASE_BASE_URL                   override the download base URL
param(
    [string]$Version = $(if ($env:AGENTC_VERSION) { $env:AGENTC_VERSION } else { "latest" }),
    [string]$InstallDir = $env:AGENTC_INSTALL_DIR
)

$ErrorActionPreference = "Stop"

function Get-AgentcVersion {
    param([string]$Path)
    try {
        $Output = & $Path --version 2>$null | Select-Object -First 1
        if ($Output -match '^agentc\s+(.+)$') { return $Matches[1].Trim() }
    } catch {
    }
    return $null
}

if (-not $InstallDir) {
    if (-not $HOME) {
        throw "HOME is not set; pass -InstallDir or set AGENTC_INSTALL_DIR."
    }
    $InstallDir = Join-Path $HOME ".local\bin"
}

$Repo = if ($env:AGENTC_REPO) { $env:AGENTC_REPO } else { "abird-ai/agentc" }
$BaseUrl = $env:AGENTC_RELEASE_BASE_URL

$Arch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture
switch ($Arch) {
    ([System.Runtime.InteropServices.Architecture]::X64)   { $Asset = "agentc-windows-x86_64" }
    ([System.Runtime.InteropServices.Architecture]::Arm64) { $Asset = "agentc-windows-aarch64" }
    default { throw "Unsupported Windows architecture: $Arch. Published: x64, arm64." }
}
$Archive = "$Asset.zip"

if (-not $BaseUrl) {
    if ($Version -eq "latest") {
        $BaseUrl = "https://github.com/$Repo/releases/latest/download"
    } else {
        $Tag = if ($Version.StartsWith("v")) { $Version } else { "v$Version" }
        $BaseUrl = "https://github.com/$Repo/releases/download/$Tag"
    }
}

$WorkDir = Join-Path ([System.IO.Path]::GetTempPath()) ("agentc-install-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $WorkDir | Out-Null

try {
    $ZipPath = Join-Path $WorkDir $Archive
    $SumPath = "$ZipPath.sha256"

    Write-Host "Downloading agentc..."
    Invoke-WebRequest -UseBasicParsing -Uri "$($BaseUrl.TrimEnd('/'))/$Archive" -OutFile $ZipPath
    Invoke-WebRequest -UseBasicParsing -Uri "$($BaseUrl.TrimEnd('/'))/$Archive.sha256" -OutFile $SumPath

    $Expected = ((Get-Content $SumPath -Raw).Trim() -split "\s+")[0].ToLowerInvariant()
    if ($Expected -notmatch '^[0-9a-f]{64}$') { throw "Invalid SHA-256 sidecar." }
    $Actual = (Get-FileHash -Algorithm SHA256 $ZipPath).Hash.ToLowerInvariant()
    if ($Expected -ne $Actual) { throw "SHA-256 verification failed." }

    $ExtractDir = Join-Path $WorkDir "extract"
    Expand-Archive -LiteralPath $ZipPath -DestinationPath $ExtractDir -Force
    $Src = Join-Path $ExtractDir "agentc.exe"
    if (-not (Test-Path -LiteralPath $Src)) { throw "archive does not contain agentc.exe" }
    $BinHash = (Get-FileHash -Algorithm SHA256 $Src).Hash.ToLowerInvariant()

    $DownloadedVersion = Get-AgentcVersion $Src
    if (-not $DownloadedVersion) { throw "downloaded binary did not report an agentc version" }
    if ($Version -ne "latest") {
        $Requested = if ($Version.StartsWith("v")) { $Version.Substring(1) } else { $Version }
        if ($DownloadedVersion -ne $Requested) {
            throw "downloaded version $DownloadedVersion does not match requested $Requested."
        }
    }

    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    $Dest = Join-Path $InstallDir "agentc.exe"
    if ((Test-Path -LiteralPath $Dest) -and
        ((Get-FileHash -Algorithm SHA256 $Dest).Hash.ToLowerInvariant() -eq $BinHash)) {
        Write-Host "agentc $DownloadedVersion is already up to date at $Dest"
        exit 0
    }

    # Replace atomically where the filesystem allows it.
    $Tmp = Join-Path $InstallDir (".agentc.exe.tmp." + [guid]::NewGuid().ToString("N"))
    Copy-Item -LiteralPath $Src -Destination $Tmp
    [System.IO.File]::Move($Tmp, $Dest, $true)

    Write-Host "Installed agentc $DownloadedVersion to $Dest"
    $UserPath = [Environment]::GetEnvironmentVariable("Path", "User")
    if ($UserPath -notlike "*$InstallDir*") {
        Write-Host "Add $InstallDir to PATH to run 'agentc'."
    }
    Write-Host "Next: agentc setup    # pick a provider, store credentials, pick a model"
}
finally {
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $WorkDir
}
