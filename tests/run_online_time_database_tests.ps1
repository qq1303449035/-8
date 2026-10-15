#requires -Version 7.0
# Starts an isolated MySQL instance; never uses configured/default databases.
param(
    [string]$MysqlRoot = 'D:\Program Files\mysql-8.0.39-winx64',
    [string]$VcVarsPath = 'D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
)
$ErrorActionPreference = 'Stop'
foreach ($path in @($MysqlRoot, $VcVarsPath)) {
    if ($path -match '["\r\n%!]' -or $path -match '[^\x00-\x7F]') { throw 'Build paths must be ASCII without batch expansion characters.' }
}
foreach ($path in @($VcVarsPath, (Join-Path $MysqlRoot 'bin\mysqld.exe'), (Join-Path $MysqlRoot 'bin\mysql.exe'))) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw 'Required compiler or MySQL executable not found.' }
}
$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) ('chh8-online-db-' + [guid]::NewGuid().ToString('N'))
if ($temporaryRoot -match '["\r\n%!?&^|<>]' -or $temporaryRoot -match '[^\x00-\x7F]') { throw 'An ASCII temporary directory is required.' }
$sourceRoot = Split-Path -Parent $PSScriptRoot
$sourceCopy = Join-Path $temporaryRoot 'source'
$testCopy = Join-Path $sourceCopy 'tests'
$dataDir = Join-Path $temporaryRoot 'mysql-data'
New-Item -ItemType Directory -Path $temporaryRoot, $sourceCopy, $testCopy, $dataDir | Out-Null
# ASCII staging avoids codepage-dependent include resolution in MSVC.
Get-ChildItem -LiteralPath $sourceRoot -File | Where-Object { $_.Extension -in '.h', '.cpp' } | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $sourceCopy
}
Copy-Item -LiteralPath (Join-Path $sourceRoot 'network') -Destination $sourceCopy -Recurse
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'online_time_database_tests.cpp') -Destination $testCopy
$executable = Join-Path $temporaryRoot 'online_time_database_tests.exe'
$object = Join-Path $temporaryRoot 'online_time_database_tests.obj'
$compile = [Diagnostics.ProcessStartInfo]::new()
$compile.FileName = $env:ComSpec
$compile.UseShellExecute = $false
$compile.CreateNoWindow = $true
$compile.RedirectStandardOutput = $true
$compile.RedirectStandardError = $true
$compile.Arguments = '/d /c ' + ('call "{0}" >nul && cl /nologo /std:c++17 /utf-8 /EHsc /W4 /WX "{1}" /I"{2}\include" /Fo"{3}" /Fe"{4}" /link /LIBPATH:"{2}\lib"' -f
    $VcVarsPath, (Join-Path $testCopy 'online_time_database_tests.cpp'), $MysqlRoot, $object, $executable)
$compiler = [Diagnostics.Process]::Start($compile)
$compilerOutput = $compiler.StandardOutput.ReadToEndAsync()
$compilerError = $compiler.StandardError.ReadToEndAsync()
$compiler.WaitForExit()
Write-Output $compilerOutput.Result
Write-Output $compilerError.Result
if ($compiler.ExitCode -ne 0) { throw 'Online time database test compilation failed.' }

function Start-HiddenTool([string]$ExecutablePath, [string[]]$Arguments) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $ExecutablePath
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    [void]$info.Environment.Remove('MYSQL_PWD')
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    return [Diagnostics.Process]::Start($info)
}
$initialize = Start-HiddenTool (Join-Path $MysqlRoot 'bin\mysqld.exe') @('--no-defaults', '--initialize-insecure',
    "--basedir=$MysqlRoot", "--datadir=$dataDir", ('--log-error=' + (Join-Path $temporaryRoot 'mysql-initialize.log')))
$initializeOutput = $initialize.StandardOutput.ReadToEndAsync()
$initializeError = $initialize.StandardError.ReadToEndAsync()
$initialize.WaitForExit()
if ($initialize.ExitCode -ne 0) { throw 'Temporary MySQL initialization failed; inspect retained test artifacts.' }
# Obtain an unused loopback port. If another process wins the bind race, the
# newly started mysqld exits and the runner fails before creating any schema.
$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
$listener.Start()
$port = ([Net.IPEndPoint]$listener.LocalEndpoint).Port
$listener.Stop()
if ($port -eq 3306 -or $port -lt 1024) { throw 'Refusing non-test port.' }
$schema = 'chh8_test_online_' + [guid]::NewGuid().ToString('N')
$server = $null
try {
    $server = Start-HiddenTool (Join-Path $MysqlRoot 'bin\mysqld.exe') @('--no-defaults', "--basedir=$MysqlRoot",
        "--datadir=$dataDir", '--bind-address=127.0.0.1', "--port=$port", '--mysqlx=0', '--skip-log-bin',
        '--performance-schema=OFF', '--innodb-buffer-pool-size=32M', '--max-connections=40',
        ('--log-error=' + (Join-Path $temporaryRoot 'mysql-server.log')))
    $serverOutput = $server.StandardOutput.ReadToEndAsync()
    $serverError = $server.StandardError.ReadToEndAsync()
    $ready = $false
    for ($attempt = 0; $attempt -lt 120; ++$attempt) {
        if ($server.HasExited) { throw 'Temporary MySQL exited before readiness; raw diagnostics omitted.' }
        $client = [Net.Sockets.TcpClient]::new()
        try { $client.Connect('127.0.0.1', $port); $ready = $true } catch { } finally { $client.Dispose() }
        if ($ready) { break }
        Start-Sleep -Milliseconds 250
    }
    if (!$ready) { throw 'Temporary MySQL readiness timed out.' }
    # Confirm this instance's datadir before performing any DDL. This guards
    # against connecting to an unrelated service if the ephemeral port raced.
    $probe = Start-HiddenTool (Join-Path $MysqlRoot 'bin\mysql.exe') @('--no-defaults', '--protocol=TCP',
        '--host=127.0.0.1', "--port=$port", '--user=root', '--password=', '--batch', '--raw', '--skip-column-names',
        '--execute', 'SELECT @@datadir')
    $probeOutput = $probe.StandardOutput.ReadToEndAsync()
    $probeError = $probe.StandardError.ReadToEndAsync()
    $probe.WaitForExit()
    $observedDataDir = $probeOutput.Result.Trim().Replace('/', '\').TrimEnd('\')
    if ($probe.ExitCode -ne 0 -or $server.HasExited -or $observedDataDir -ne $dataDir.TrimEnd('\')) {
        throw 'Isolated instance identity could not be verified; no schema created.'
    }
    $create = Start-HiddenTool (Join-Path $MysqlRoot 'bin\mysql.exe') @('--no-defaults', '--protocol=TCP',
        '--host=127.0.0.1', "--port=$port", '--user=root', '--password=', '--execute',
        ('CREATE DATABASE `{0}` CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci' -f $schema))
    $createOutput = $create.StandardOutput.ReadToEndAsync()
    $createError = $create.StandardError.ReadToEndAsync()
    $create.WaitForExit()
    if ($create.ExitCode -ne 0) { throw 'Isolated schema creation failed; raw diagnostics omitted.' }
    $run = [Diagnostics.ProcessStartInfo]::new()
    $run.FileName = $executable
    $run.UseShellExecute = $false
    $run.CreateNoWindow = $true
    $run.RedirectStandardOutput = $true
    $run.RedirectStandardError = $true
    $run.Environment['CHH_DB_HOST'] = '127.0.0.1'
    $run.Environment['CHH_DB_PORT'] = [string]$port
    $run.Environment['CHH_DB_USER'] = 'root'
    $run.Environment['CHH_DB_NAME'] = $schema
    # Explicit empty entry is mandatory; _putenv_s would remove it on Windows.
    $run.Environment['CHH_DB_PASSWORD'] = ''
    $run.Environment['PATH'] = (Join-Path $MysqlRoot 'lib') + ';' + (Join-Path $MysqlRoot 'bin') + ';' + $env:PATH
    $test = [Diagnostics.Process]::Start($run)
    $testOutput = $test.StandardOutput.ReadToEndAsync()
    $testError = $test.StandardError.ReadToEndAsync()
    $test.WaitForExit()
    Write-Output $testOutput.Result
    Write-Output $testError.Result
    if ($test.ExitCode -ne 0) { throw "Online time database tests failed with exit code $($test.ExitCode)." }
} finally {
    # Only stop the exact child process created here; never find/stop a service
    # by executable name, port, a PID file, or another process's identity.
    if ($null -ne $server -and !$server.HasExited) { $server.Kill(); $server.WaitForExit() }
    Write-Output ('Retained isolated test artifacts: ' + $temporaryRoot)
}
