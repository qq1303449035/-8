param(
    [string]$VcVarsPath = 'D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat',
    [string]$ServerSource = (Join-Path $PSScriptRoot '..\appServer.cpp'),
    [string]$SessionsSource = (Join-Path $PSScriptRoot '..\sessions.h'),
    [string]$ProtocolSource = (Join-Path $PSScriptRoot '..\network\protocol.h')
)
# Types 179 (game_hero_delay_set), 180 (game_move_distance_set) and 181 (game_hero_keys_set) are retired: the three
# hero settings now travel over the local LA<->DLL link (chh8/local_link.h). The server must reject them like any
# unknown type and keep no relay state for them.
$ErrorActionPreference = 'Stop'
if (!(Test-Path -LiteralPath $VcVarsPath -PathType Leaf)) {
    throw 'vcvars64.bat was not found. Pass -VcVarsPath for the installed Visual Studio toolchain.'
}
$build = Join-Path ([IO.Path]::GetTempPath()) ('chh8-server-retired-hero-types-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $build, (Join-Path $build 'tests'), (Join-Path $build 'network') | Out-Null
# Copy fixtures to an ASCII temp path so cmd.exe cannot corrupt repository paths.
$server = [IO.File]::ReadAllText($ServerSource)
$sessions = [IO.File]::ReadAllText($SessionsSource)
$protocol = [IO.File]::ReadAllText($ProtocolSource)

# Source checks: no relay code or state may survive anywhere on the server.
foreach ($name in @('game_hero_delay_set', 'game_move_distance_set', 'game_hero_keys_set',
        'hero_delay', 'move_distance', 'hero_keys')) {
    if ($server.Contains($name)) { throw "appServer.cpp still references the retired relay: $name" }
}
foreach ($name in @('hero_delay', 'move_distance', 'hero_keys', 'hero_combo_key', 'hero_toggle_key', 'hero_lane_key',
        'HeroDelay', 'MoveDistance', 'HeroKeys')) {
    if ($sessions.Contains($name)) { throw "sessions.h still keeps retired relay state: $name" }
}
foreach ($name in @('game_hero_delay', 'game_move_distance', 'game_hero_keys', 'game_hero_combo_key',
        'game_hero_toggle_key', 'game_hero_lane_key', 'valid_game_hero_key')) {
    if ($protocol.Contains($name)) { throw "network/protocol.h still defines a retired name: $name" }
}
if (!$protocol.Contains('// 179-181 retired: moved to the local LA<->DLL link (chh8/local_link.h); never reuse')) {
    throw 'network/protocol.h must keep the retirement comment for types 179-181'
}
if (!$protocol.Contains('uint32_t(type()) <= uint32_t(Type::game_auto_afk_set); }')) {
    throw 'Packet::valid() must use Type::game_auto_afk_set as its upper bound'
}

function Slice-Source([string]$startMarker, [string]$endMarker) {
    $start = $server.IndexOf($startMarker, [StringComparison]::Ordinal)
    if ($start -lt 0) { throw "Missing source marker: $startMarker" }
    $end = $server.IndexOf($endMarker, $start + $startMarker.Length, [StringComparison]::Ordinal)
    if ($end -lt 0) { throw "Missing source marker: $endMarker" }
    return $server.Substring($start, $end - $start)
}
$senders = Slice-Source 'bool send_packet(' 'bool send_chat_result('
# The real gate every received frame passes before any dispatch (primary and game endpoint alike).
$gate = Slice-Source '        if (!packet.valid()) {' '        last_received = Clock::now();'
if (!$gate.Contains('send_error(stream, chh::wire::Error::protocol); break;')) {
    throw 'Extracted packet gate must answer an invalid packet with a protocol error and stop the dispatch loop'
}
# The handlers that now sit where the 179-181 handlers used to be (auto AFK is type 178, the highest valid type).
$handler = Slice-Source '        } else if (packet.type() == chh::wire::Type::game_auto_afk_set)' `
    '        } else if (packet.type() == chh::wire::Type::game_auto_glove_set)'
$handler = '        if' + $handler.Substring('        } else if'.Length)
foreach ($type in @('game_auto_afk_set', 'game_skin_block_set', 'game_attack_range_set')) {
    if (!$handler.Contains("packet.type() == chh::wire::Type::$type)")) { throw "Extracted handler is missing $type" }
}
# The send blocks that now follow auto AFK in the game endpoint's forwarding loop.
$forward = Slice-Source '            const auto auto_afk = online.auto_afk_command_snapshot(member.binding);' `
    '            bool guisuo_exclusions_sent = true;'
foreach ($type in @('game_auto_afk_set', 'game_skin_block_set', 'game_attack_range_set')) {
    if (!$forward.Contains("send_command(stream, chh::wire::Type::$type,")) { throw "Extracted forwarding is missing $type" }
}
$utf8 = [Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText((Join-Path $build 'retired_hero_types_senders.inc'), $senders, $utf8)
[IO.File]::WriteAllText((Join-Path $build 'retired_hero_types_gate.inc'), $gate, $utf8)
[IO.File]::WriteAllText((Join-Path $build 'retired_hero_types_handler.inc'), $handler, $utf8)
[IO.File]::WriteAllText((Join-Path $build 'retired_hero_types_forward.inc'), $forward, $utf8)
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'retired_hero_types_server_tests.cpp') -Destination (Join-Path $build 'tests')
Copy-Item -LiteralPath $ProtocolSource -Destination (Join-Path $build 'network')
Copy-Item -LiteralPath $SessionsSource -Destination $build
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
    'cl /nologo /std:c++17 /utf-8 /EHsc /W4 /I . tests\retired_hero_types_server_tests.cpp /Fe:retired_hero_types_server_tests.exe',
    'exit /b %errorlevel%'
)
[IO.File]::WriteAllLines($command, $lines, [Text.Encoding]::ASCII)
& $env:ComSpec /d /c $command
if ($LASTEXITCODE -ne 0) { throw "Server retired hero types test compilation failed with exit code $LASTEXITCODE." }
& (Join-Path $build 'retired_hero_types_server_tests.exe')
$result = $LASTEXITCODE
Write-Output ('Test build artifacts: ' + $build)
if ($result -ne 0) { throw "Server retired hero types tests failed with exit code $result." }
