param([string]$MySqlRoot = 'D:\Program Files\mysql-8.0.39-winx64')
$ErrorActionPreference = 'Stop'
$buildInfo = [System.Diagnostics.ProcessStartInfo]::new()
$buildInfo.FileName = 'D:\C++\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe'
$buildInfo.UseShellExecute = $false
$buildInfo.CreateNoWindow = $true
$buildInfo.Environment.Clear()
foreach ($entry in [Environment]::GetEnvironmentVariables().GetEnumerator()) { $buildInfo.Environment[$entry.Key.ToUpperInvariant()] = $entry.Value }
$outputDir = Join-Path $PSScriptRoot 'x64\Release\'
foreach ($arg in @((Join-Path $PSScriptRoot '茶会服务器8.vcxproj'),'/t:Build','/p:Configuration=Release','/p:Platform=x64',"/p:OutDir=$outputDir","/p:IntDir=$PSScriptRoot\x64\obj\",'/nologo','/v:minimal',"/flp:logfile=$PSScriptRoot\build.log;verbosity=minimal")) { $buildInfo.ArgumentList.Add($arg) }
$process = [System.Diagnostics.Process]::Start($buildInfo)
$process.WaitForExit()
if ($process.ExitCode -ne 0) { exit $process.ExitCode }
foreach ($dependency in @('lib\libmysql.dll','bin\libssl-3-x64.dll','bin\libcrypto-3-x64.dll')) {
 Copy-Item -LiteralPath (Join-Path $MySqlRoot $dependency) -Destination $outputDir -Force
}
$runtimeRoot = 'D:\C++\Microsoft Visual Studio\2022\Enterprise\VC\Redist\MSVC\14.44.35112\x64\Microsoft.VC143.CRT'
Get-ChildItem -LiteralPath $runtimeRoot -Filter '*.dll' | ForEach-Object {
 Copy-Item -LiteralPath $_.FullName -Destination $outputDir -Force
}
$pfxSource = Join-Path $PSScriptRoot 'deployment\server.pfx'
$pfxOutput = Join-Path $outputDir 'server.pfx'
Copy-Item -LiteralPath $pfxSource -Destination $pfxOutput -Force
$ownerAccess = '*' + [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value + ':(F)'
& icacls.exe $pfxOutput '/inheritance:r' '/grant:r' $ownerAccess '*S-1-5-18:(F)' | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Unable to restrict private certificate access.' }
$launcherSource = Join-Path $PSScriptRoot 'deployment\start-server.cmd'
Copy-Item -LiteralPath $launcherSource -Destination $outputDir -Force

# Produce a clean, versioned directory so an old EXE or intermediate file cannot be deployed by mistake.
$releaseStamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$packageRoot = Join-Path $PSScriptRoot ('packages\chh8-server-' + $releaseStamp)
New-Item -ItemType Directory -Path $packageRoot -ErrorAction Stop | Out-Null
foreach ($filename in @('茶会服务器8.exe','libmysql.dll','libssl-3-x64.dll','libcrypto-3-x64.dll','server.pfx','start-server.cmd')) {
 Copy-Item -LiteralPath (Join-Path $outputDir $filename) -Destination $packageRoot
}
Get-ChildItem -LiteralPath $runtimeRoot -Filter '*.dll' | ForEach-Object {
 Copy-Item -LiteralPath $_.FullName -Destination $packageRoot
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'README.md') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'UPDATE_NOTES.md') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'GAME_PROTOCOL.md') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'friends_schema.sql') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'temporary_chat_indexes.sql') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'account_preferences.sql') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'temporary_chat_clear.sql') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'FRIENDS_MIGRATION.md') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'avatar_schema.sql') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'AVATAR_MIGRATION.md') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'media_schema.sql') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'media_original_migration.sql') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'MEDIA_MIGRATION.md') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'recall_schema.sql') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'RECALL_MIGRATION.md') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'local_history_migration.sql') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'LOCAL_HISTORY_MIGRATION.md') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'private_media_migration.sql') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'PRIVATE_MEDIA_MIGRATION.md') -Destination $packageRoot
$manifest = Get-ChildItem -LiteralPath $packageRoot -File | ForEach-Object {
 $hash = Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256
 '{0}  {1}' -f $hash.Hash, $_.Name
}
$manifest | Set-Content -LiteralPath (Join-Path $packageRoot 'SHA256SUMS.txt') -Encoding utf8
$packageZip = $packageRoot + '.zip'
Compress-Archive -LiteralPath $packageRoot -DestinationPath $packageZip -CompressionLevel Optimal
foreach ($privateFile in @((Join-Path $packageRoot 'server.pfx'), $packageZip)) {
 & icacls.exe $privateFile '/inheritance:r' '/grant:r' $ownerAccess '*S-1-5-18:(F)' | Out-Null
 if ($LASTEXITCODE -ne 0) { throw 'Unable to restrict private deployment file access.' }
}
Write-Output ('Server deployment package: ' + $packageZip)
