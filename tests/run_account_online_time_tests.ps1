param(
    [string]$VcVarsPath = 'D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat',
    [string]$ServerSource = (Join-Path $PSScriptRoot '..\appServer.cpp')
)
$ErrorActionPreference = 'Stop'
if (!(Test-Path -LiteralPath $VcVarsPath -PathType Leaf)) {
    throw 'vcvars64.bat was not found. Pass -VcVarsPath for the installed Visual Studio toolchain.'
}
$build = Join-Path ([IO.Path]::GetTempPath()) ('chh8-account-online-time-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $build, (Join-Path $build 'tests'), (Join-Path $build 'network') | Out-Null
# Exercise production cache and packet sender with an in-memory database loader and TLS destination.
$server = [IO.File]::ReadAllText($ServerSource)
$startMarker = 'struct AccountOnlineTime {'
$endMarker = 'bool send_account_result('
$start = $server.IndexOf($startMarker, [StringComparison]::Ordinal)
if ($start -lt 0) { throw "Missing source marker: $startMarker" }
$end = $server.IndexOf($endMarker, $start + $startMarker.Length, [StringComparison]::Ordinal)
if ($end -lt 0) { throw "Missing source marker: $endMarker" }
[IO.File]::WriteAllText((Join-Path $build 'account_sender.inc'), $server.Substring($start, $end - $start), [Text.UTF8Encoding]::new($false))
foreach ($part in @(
    @('social_state.inc', 'struct SocialState {', '// Only a successful result'),
    @('friends_sender.inc', 'bool send_friends(', 'bool refresh_social('),
    @('social_join.inc', 'bool parse_social_join(', 'bool send_private_stored(')
)) {
    $start = $server.IndexOf($part[1], [StringComparison]::Ordinal)
    if ($start -lt 0) { throw ('Missing source marker: ' + $part[1]) }
    $end = $server.IndexOf($part[2], $start + $part[1].Length, [StringComparison]::Ordinal)
    if ($end -lt 0) { throw ('Missing source marker: ' + $part[2]) }
    [IO.File]::WriteAllText((Join-Path $build $part[0]), $server.Substring($start, $end - $start), [Text.UTF8Encoding]::new($false))
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'account_online_time_tests.cpp') -Destination (Join-Path $build 'tests')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\network\protocol.h') -Destination (Join-Path $build 'network')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\sessions.h'), (Join-Path $PSScriptRoot '..\MySQL.h') -Destination $build
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\chat_room.h'), (Join-Path $PSScriptRoot '..\social_diagnostics.h') -Destination $build
foreach ($path in @($VcVarsPath, $build)) {
    if ($path -match '["\r\n%!]' -or $path -match '[^\x00-\x7F]') {
        throw 'This test launcher needs ASCII tool and temp paths without batch expansion characters.'
    }
}
$command = Join-Path $build 'compile.cmd'
$lines = @(
    '@echo off',
    ('call "{0}" >nul' -f $VcVarsPath),
    'if errorlevel 1 exit /b %errorlevel%',
    ('pushd "{0}"' -f $build),
    'cl /nologo /std:c++17 /utf-8 /EHsc /W4 /WX /I . tests\account_online_time_tests.cpp /Fe:account_online_time_tests.exe',
    'exit /b %errorlevel%'
)
[IO.File]::WriteAllLines($command, $lines, [Text.Encoding]::ASCII)
& $env:ComSpec /d /c $command
if ($LASTEXITCODE -ne 0) { throw "Account online time test compilation failed with exit code $LASTEXITCODE." }
& (Join-Path $build 'account_online_time_tests.exe')
$result = $LASTEXITCODE
Write-Output ('Test build artifacts: ' + $build)
if ($result -ne 0) { throw "Account online time tests failed with exit code $result." }
