param(
    [string]$VcVarsPath = 'D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat',
    [string]$ServerSource = (Join-Path $PSScriptRoot '..\appServer.cpp')
)
$ErrorActionPreference = 'Stop'
if (!(Test-Path -LiteralPath $VcVarsPath -PathType Leaf)) {
    throw 'vcvars64.bat was not found. Pass -VcVarsPath for the installed Visual Studio toolchain.'
}
$build = Join-Path ([IO.Path]::GetTempPath()) ('chh8-server-auto-afk-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $build, (Join-Path $build 'tests'), (Join-Path $build 'network') | Out-Null
# Copy fixtures to an ASCII temp path so cmd.exe cannot corrupt repository paths.
$server = [IO.File]::ReadAllText($ServerSource)
function Slice-Source([string]$startMarker, [string]$endMarker) {
    $start = $server.IndexOf($startMarker, [StringComparison]::Ordinal)
    if ($start -lt 0) { throw "Missing source marker: $startMarker" }
    $end = $server.IndexOf($endMarker, $start + $startMarker.Length, [StringComparison]::Ordinal)
    if ($end -lt 0) { throw "Missing source marker: $endMarker" }
    return $server.Substring($start, $end - $start)
}
$senders = Slice-Source 'bool send_packet(' 'bool send_chat_result('
$handler = Slice-Source '        } else if (packet.type() == chh::wire::Type::game_auto_afk_set)' `
    '        } else if (packet.type() == chh::wire::Type::game_move_distance_set)'
$handler = '        if' + $handler.Substring('        } else if'.Length) + '        }'
$forward = Slice-Source '            const auto auto_afk = online.auto_afk_command_snapshot(member.binding);' `
    '            const auto move_distance = online.move_distance_command_snapshot(member.binding);'
$utf8 = [Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText((Join-Path $build 'auto_afk_senders.inc'), $senders, $utf8)
[IO.File]::WriteAllText((Join-Path $build 'auto_afk_handler.inc'), $handler, $utf8)
[IO.File]::WriteAllText((Join-Path $build 'auto_afk_forward.inc'), $forward, $utf8)
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'auto_afk_server_tests.cpp') -Destination (Join-Path $build 'tests')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\network\protocol.h') -Destination (Join-Path $build 'network')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\sessions.h') -Destination $build
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
    'cl /nologo /std:c++17 /utf-8 /EHsc /W4 /I . tests\auto_afk_server_tests.cpp /Fe:auto_afk_server_tests.exe',
    'exit /b %errorlevel%'
)
[IO.File]::WriteAllLines($command, $lines, [Text.Encoding]::ASCII)
& $env:ComSpec /d /c $command
if ($LASTEXITCODE -ne 0) { throw "Server auto AFK test compilation failed with exit code $LASTEXITCODE." }
& (Join-Path $build 'auto_afk_server_tests.exe')
$result = $LASTEXITCODE
Write-Output ('Test build artifacts: ' + $build)
if ($result -ne 0) { throw "Server auto AFK tests failed with exit code $result." }
