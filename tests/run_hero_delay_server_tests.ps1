param(
    [string]$VcVarsPath = 'D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat',
    [string]$ServerSource = (Join-Path $PSScriptRoot '..\appServer.cpp')
)
$ErrorActionPreference = 'Stop'
if (!(Test-Path -LiteralPath $VcVarsPath -PathType Leaf)) {
    throw 'vcvars64.bat was not found. Pass -VcVarsPath for the installed Visual Studio toolchain.'
}
$build = Join-Path ([IO.Path]::GetTempPath()) ('chh8-server-hero-delay-' + [guid]::NewGuid().ToString('N'))
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
$handler = Slice-Source '        } else if (packet.type() == chh::wire::Type::game_move_distance_set)' `
    '        } else if (packet.type() == chh::wire::Type::game_skin_block_set)'
$handler = '        if' + $handler.Substring('        } else if'.Length) + '        }'
$forward = Slice-Source '            const auto move_distance = online.move_distance_command_snapshot(member.binding);' `
    '            const auto skin_block = online.skin_block_command_snapshot(member.binding);'
# Types 180 (move distance), 181 (hero keys) and 179 (run delay) must all sit inside the extracted slices.
foreach ($type in @('game_move_distance_set', 'game_hero_keys_set', 'game_hero_delay_set')) {
    if (!$handler.Contains("packet.type() == chh::wire::Type::$type)")) { throw "Extracted handler is missing $type" }
    if (!$forward.Contains("send_command(stream, chh::wire::Type::$type,")) { throw "Extracted forwarding is missing $type" }
}
# Type 181 carries exactly three keys (12 bytes): the handler reads and the forwarder sends combo, toggle, then lane key.
if (!$handler.Contains('!body.number(combo_key) || !body.number(toggle_key) || !body.number(lane_key) || !body.done()')) {
    throw 'Extracted hero keys handler must read exactly the combo, toggle and lane keys'
}
if (!$forward.Contains('{hero_keys.combo_key, hero_keys.toggle_key, hero_keys.lane_key}')) {
    throw 'Extracted hero keys forwarding must send the combo, toggle and lane keys'
}
$utf8 = [Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText((Join-Path $build 'hero_delay_senders.inc'), $senders, $utf8)
[IO.File]::WriteAllText((Join-Path $build 'hero_delay_handler.inc'), $handler, $utf8)
[IO.File]::WriteAllText((Join-Path $build 'hero_delay_forward.inc'), $forward, $utf8)
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'hero_delay_server_tests.cpp') -Destination (Join-Path $build 'tests')
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
    'cl /nologo /std:c++17 /utf-8 /EHsc /W4 /I . tests\hero_delay_server_tests.cpp /Fe:hero_delay_server_tests.exe',
    'exit /b %errorlevel%'
)
[IO.File]::WriteAllLines($command, $lines, [Text.Encoding]::ASCII)
& $env:ComSpec /d /c $command
if ($LASTEXITCODE -ne 0) { throw "Server hero delay, move distance and hero keys test compilation failed with exit code $LASTEXITCODE." }
& (Join-Path $build 'hero_delay_server_tests.exe')
$result = $LASTEXITCODE
Write-Output ('Test build artifacts: ' + $build)
if ($result -ne 0) { throw "Server hero delay, move distance and hero keys tests failed with exit code $result." }
