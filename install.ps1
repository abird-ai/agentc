# agentc installer — Windows (x86-64)
#
#   powershell -ExecutionPolicy Bypass -c "irm https://raw.githubusercontent.com/<repo>/main/install.ps1 | iex"
#
# Parameters / environment:
#   -Repo     GitHub repo   (default: pvl/agentc)
#   -Version  tag like v0.1.0, or "latest" (default)
#   -Dir      install dir   (default: %LOCALAPPDATA%\agentc)
param(
    [string]$Repo = "pvl/agentc",
    [string]$Version = "latest",
    [string]$Dir = "$env:LOCALAPPDATA\agentc"
)
$ErrorActionPreference = "Stop"

$asset = "agentc-windows-x86_64.zip"
if ($Version -eq "latest") {
    $url = "https://github.com/$Repo/releases/latest/download/$asset"
} else {
    $url = "https://github.com/$Repo/releases/download/$Version/$asset"
}

$tmp = Join-Path $env:TEMP ("agentc-install-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
try {
    Write-Host "agentc: downloading $url"
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile (Join-Path $tmp $asset)
    Expand-Archive -Path (Join-Path $tmp $asset) -DestinationPath $tmp -Force
    $exe = Join-Path $tmp "agentc.exe"
    if (-not (Test-Path $exe)) { throw "archive did not contain agentc.exe" }

    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    Copy-Item $exe (Join-Path $Dir "agentc.exe") -Force

    $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
    if ($userPath -notlike "*$Dir*") {
        [Environment]::SetEnvironmentVariable("Path", "$userPath;$Dir", "User")
        Write-Host "agentc: added $Dir to the user PATH (restart your shell)"
    }
    Write-Host "agentc: installed $Dir\agentc.exe"
    & (Join-Path $Dir "agentc.exe") --version
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
