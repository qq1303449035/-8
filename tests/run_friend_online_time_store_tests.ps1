param(
    [string]$VcVarsPath = 'D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
)
$ErrorActionPreference = 'Stop'
if (!(Test-Path -LiteralPath $VcVarsPath -PathType Leaf)) { throw 'Visual Studio setup not found.' }
$build = Join-Path ([IO.Path]::GetTempPath()) ('chh8-friend-online-store-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $build, (Join-Path $build 'tests'), (Join-Path $build 'network') | Out-Null
$source = [IO.File]::ReadAllText((Join-Path $PSScriptRoot '..\friends_store.h'))
function Copy-SourceRange([string]$StartMarker, [string]$EndMarker, [string]$Destination) {
    $start = $source.IndexOf($StartMarker, [StringComparison]::Ordinal)
    if ($start -lt 0) { throw "Missing source marker: $StartMarker" }
    $end = $source.IndexOf($EndMarker, $start + $StartMarker.Length, [StringComparison]::Ordinal)
    if ($end -lt 0) { throw "Missing source marker: $EndMarker" }
    [IO.File]::WriteAllText((Join-Path $build $Destination), $source.Substring($start, $end - $start), [Text.UTF8Encoding]::new($false))
}
Copy-SourceRange 'bool social_uint64(' 'bool social_id(' 'friend_online_uint64.inc'
Copy-SourceRange 'bool list_friend_online_times(' 'wire::SocialResult list_friends(' 'friend_online_store.inc'
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'friend_online_time_store_tests.cpp') -Destination (Join-Path $build 'tests')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\MySQL.h') -Destination $build
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\network\protocol.h') -Destination (Join-Path $build 'network')
foreach ($path in @($VcVarsPath, $build)) {
    if ($path -match '["\r\n%!]' -or $path -match '[^\x00-\x7F]') { throw 'ASCII tool and build paths required.' }
}
$command = Join-Path $build 'compile.cmd'
$lines = @('@echo off', ('call "{0}" >nul' -f $VcVarsPath), 'if errorlevel 1 exit /b %errorlevel%',
    ('pushd "{0}"' -f $build),
    'cl /nologo /std:c++17 /utf-8 /EHsc /W4 /WX /I . tests\friend_online_time_store_tests.cpp /Fe:friend_online_time_store_tests.exe',
    'exit /b %errorlevel%')
[IO.File]::WriteAllLines($command, $lines, [Text.Encoding]::ASCII)
& $env:ComSpec /d /c $command
if ($LASTEXITCODE -ne 0) { throw 'Friend online time store test compilation failed.' }
& (Join-Path $build 'friend_online_time_store_tests.exe')
$result = $LASTEXITCODE
Write-Output ('Test build artifacts: ' + $build)
if ($result -ne 0) { throw 'Friend online time store tests failed.' }
