param([Parameter(Mandatory=$true)][string]$Source,[Parameter(Mandatory=$true)][string]$OutputDir,[string]$Version='0.3.4',[string]$CompilerPath='')
$ErrorActionPreference='Stop'
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$buildStamp=Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$target=Join-Path $OutputDir ("ccrIDE-v$Version.exe")
$serial=0
while(Test-Path -LiteralPath $target) { $serial++; $target=Join-Path $OutputDir ("ccrIDE-v$Version-$serial.exe") }
[System.IO.File]::Copy((Resolve-Path -LiteralPath $Source).Path,$target,$false)
$stream=[IO.File]::OpenRead($target)
try { $digest=[Security.Cryptography.SHA256]::Create(); $hash=[BitConverter]::ToString($digest.ComputeHash($stream)).Replace('-','').ToLowerInvariant() }
finally { $stream.Dispose(); if($digest) { $digest.Dispose() } }
if($CompilerPath -and !(Test-Path -LiteralPath (Join-Path $OutputDir 'libwinpthread-1.dll'))) { Copy-Item -LiteralPath (Join-Path (Split-Path $CompilerPath) 'libwinpthread-1.dll') -Destination $OutputDir }
$record=@{version=$Version;buildTime=(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz');exe=$target;sha256=$hash}
$record | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDir 'latest-build.json') -Encoding UTF8
Write-Output "Build artifact: $target"
