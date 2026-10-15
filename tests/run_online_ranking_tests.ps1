param(
    [string]$VcVarsPath = 'D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat',
    [string]$ServerSource = (Join-Path $PSScriptRoot '..\appServer.cpp')
)
$ErrorActionPreference = 'Stop'
if (!(Test-Path -LiteralPath $VcVarsPath -PathType Leaf)) {
    throw 'vcvars64.bat was not found. Pass -VcVarsPath for the installed Visual Studio toolchain.'
}
$build = Join-Path ([IO.Path]::GetTempPath()) ('chh8-online-ranking-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $build, (Join-Path $build 'tests'), (Join-Path $build 'network') | Out-Null
# Exercise the production sender; only send_packet and its TLS destination are faked.
# An ASCII build path keeps cmd.exe away from the Unicode repository path.
$server = [IO.File]::ReadAllText($ServerSource)
$startMarker = 'bool send_hero_rank_page('
$endMarker = 'bool send_game_notes('
$start = $server.IndexOf($startMarker, [StringComparison]::Ordinal)
if ($start -lt 0) { throw "Missing source marker: $startMarker" }
$end = $server.IndexOf($endMarker, $start + $startMarker.Length, [StringComparison]::Ordinal)
if ($end -lt 0) { throw "Missing source marker: $endMarker" }
$sender = $server.Substring($start, $end - $start)
[IO.File]::WriteAllText((Join-Path $build 'rank_sender.inc'), $sender, [Text.UTF8Encoding]::new($false))
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'online_ranking_tests.cpp') -Destination (Join-Path $build 'tests')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\network\protocol.h') -Destination (Join-Path $build 'network')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\hero_ranking.h'), (Join-Path $PSScriptRoot '..\MySQL.h') -Destination $build
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
    'cl /nologo /std:c++17 /utf-8 /EHsc /W4 /WX /I . tests\online_ranking_tests.cpp /Fe:online_ranking_tests.exe',
    'exit /b %errorlevel%'
)
[IO.File]::WriteAllLines($command, $lines, [Text.Encoding]::ASCII)
& $env:ComSpec /d /c $command
if ($LASTEXITCODE -ne 0) { throw "Online ranking test compilation failed with exit code $LASTEXITCODE." }
& (Join-Path $build 'online_ranking_tests.exe')
$result = $LASTEXITCODE
Write-Output ('Test build artifacts: ' + $build)
if ($result -ne 0) { throw "Online ranking tests failed with exit code $result." }
