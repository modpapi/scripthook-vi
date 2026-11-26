# Installs the shvi CLI (a single exe) into ~\.shvi\bin and the matching ScriptHook VI SDK.
#   irm https://github.com/modpapi/scripthook-vi/releases/latest/download/install.ps1 | iex
#   & ([scriptblock]::Create((irm <url>/install.ps1))) -Version 0.2.0      a specific release
# Env: SHVI_REPO (owner/name), SHVI_HOME (default ~\.shvi).
param([string]$Version = "")
$ErrorActionPreference = "Stop"

$repo = if ($env:SHVI_REPO) { $env:SHVI_REPO } else { "modpapi/scripthook-vi" }
$homeDir = if ($env:SHVI_HOME) { $env:SHVI_HOME } else { Join-Path $HOME ".shvi" }
$Version = $Version.TrimStart("v")
$base = if ($Version) { "https://github.com/$repo/releases/download/v$Version" } else { "https://github.com/$repo/releases/latest/download" }

$bin = Join-Path $homeDir "bin"
New-Item -ItemType Directory -Force $bin | Out-Null
$exe = Join-Path $bin "shvi.exe"
Write-Host "› downloading shvi-windows-x64.exe from $base"
Invoke-WebRequest -UseBasicParsing "$base/shvi-windows-x64.exe" -OutFile "$exe.tmp"
Move-Item -Force "$exe.tmp" $exe
$v = (& $exe --version).Trim()
Write-Host "✓ shvi $v installed: $exe"
& $exe sdk install $v

$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
if (-not ($userPath -split ";" | Where-Object { $_ -eq $bin })) {
  [Environment]::SetEnvironmentVariable("Path", "$bin;$userPath", "User")
  Write-Host "Added $bin to your user PATH (open a new terminal)."
}
Write-Host "Next: shvi doctor   then   shvi new"
