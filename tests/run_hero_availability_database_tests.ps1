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
$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) ('chh8-heroes-db-' + [guid]::NewGuid().ToString('N'))
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
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'hero_availability_database_tests.cpp') -Destination $testCopy
$executable = Join-Path $temporaryRoot 'hero_availability_database_tests.exe'
$object = Join-Path $temporaryRoot 'hero_availability_database_tests.obj'
$compile = [Diagnostics.ProcessStartInfo]::new()
$compile.FileName = $env:ComSpec
$compile.UseShellExecute = $false
$compile.CreateNoWindow = $true
$compile.RedirectStandardOutput = $true
$compile.RedirectStandardError = $true
$compile.Arguments = '/d /c ' + ('call "{0}" >nul && cl /nologo /std:c++17 /utf-8 /EHsc /W4 /WX "{1}" /I"{2}\include" /Fo"{3}" /Fe"{4}" /link /LIBPATH:"{2}\lib"' -f
    $VcVarsPath, (Join-Path $testCopy 'hero_availability_database_tests.cpp'), $MysqlRoot, $object, $executable)
$compiler = [Diagnostics.Process]::Start($compile)
$compilerOutput = $compiler.StandardOutput.ReadToEndAsync()
$compilerError = $compiler.StandardError.ReadToEndAsync()
$compiler.WaitForExit()
Write-Output $compilerOutput.Result
Write-Output $compilerError.Result
if ($compiler.ExitCode -ne 0) { throw 'Hero availability database test compilation failed.' }

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
$schema = 'chh8_test_heroes_' + [guid]::NewGuid().ToString('N')
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
    function Invoke-TestSql([string]$SqlText) {
        $info = [Diagnostics.ProcessStartInfo]::new()
        $info.FileName = Join-Path $MysqlRoot 'bin\mysql.exe'
        $info.UseShellExecute = $false; $info.CreateNoWindow = $true
        $info.RedirectStandardInput = $true; $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
        $info.StandardInputEncoding = [Text.UTF8Encoding]::new($false)
        $info.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
        [void]$info.Environment.Remove('MYSQL_PWD')
        foreach ($argument in @('--no-defaults','--protocol=TCP','--host=127.0.0.1',"--port=$port",'--user=root','--password=',
            "--database=$schema",'--default-character-set=utf8mb4','--batch','--raw','--skip-column-names')) { $info.ArgumentList.Add($argument) }
        $process = [Diagnostics.Process]::Start($info)
        $output = $process.StandardOutput.ReadToEndAsync(); $errors = $process.StandardError.ReadToEndAsync()
        $process.StandardInput.Write($SqlText); $process.StandardInput.Close(); $process.WaitForExit()
        if ($process.ExitCode -ne 0) { throw 'Isolated migration SQL failed; raw diagnostics omitted.' }
        return $output.Result.Trim()
    }
    $migrationPath = Join-Path $sourceRoot 'hero_permissions_migration.sql'
    Copy-Item -LiteralPath $migrationPath -Destination (Join-Path $temporaryRoot 'hero_permissions_migration.sql')
    $migration = [IO.File]::ReadAllText($migrationPath)
    $seeds = [regex]::Matches($migration, '(?m)^UPDATE herouse SET permissionID=(\d+) WHERE heroID=(\d+) AND permissionID IS NULL;\r?$')
    if ($seeds.Count -ne 246) { throw 'Expected exactly 246 migration seed mappings.' }
    $fixture = "CREATE TABLE herouse(ID INT AUTO_INCREMENT PRIMARY KEY,heroID INT NULL,heroName VARCHAR(255)," +
        "jjcNum INT DEFAULT 0,dldNum INT DEFAULT 0,jjcWin INT DEFAULT 0,jjcLos INT DEFAULT 0,dldWin INT DEFAULT 0,dldLos INT DEFAULT 0) ENGINE=InnoDB;"
    foreach ($seed in $seeds) {
        $id = [uint32]$seed.Groups[2].Value
        $fixture += "INSERT INTO herouse(heroID,heroName,jjcNum,dldNum,jjcWin,jjcLos,dldWin,dldLos) VALUES($id,'自定义英雄 $id',7,8,9,10,11,12);"
    }
    $fixture += "INSERT INTO herouse(heroID,heroName,jjcNum) VALUES(60000,'未配置新英雄',777);"
    [void](Invoke-TestSql $fixture)
    $preservedColumns = 'SELECT ID,heroID,heroName,jjcNum,dldNum,jjcWin,jjcLos,dldWin,dldLos FROM herouse ORDER BY ID;'
    $beforeMigration = Invoke-TestSql $preservedColumns
    [void](Invoke-TestSql $migration)
    if ((Invoke-TestSql $preservedColumns) -cne $beforeMigration) { throw 'Migration changed names or statistics.' }
    $actual = (Invoke-TestSql 'SELECT CONCAT(heroID,'':'',COALESCE(CAST(permissionID AS CHAR),''NULL'')) FROM herouse ORDER BY heroID;') -split '\r?\n'
    foreach ($seed in $seeds) {
        $expected = $seed.Groups[2].Value + ':' + $seed.Groups[1].Value
        if ($actual -cnotcontains $expected) { throw 'A migrated permission differs from its exact seed.' }
    }
    if ((Invoke-TestSql 'SELECT COUNT(*) FROM herouse WHERE permissionID=0;') -ne '68' -or $actual -cnotcontains '60000:NULL') {
        throw 'Migration free count or unknown-hero default mismatch.'
    }
    [void](Invoke-TestSql 'UPDATE herouse SET permissionID=9876 WHERE heroID=69;')
    [void](Invoke-TestSql $migration)
    if ((Invoke-TestSql 'SELECT permissionID FROM herouse WHERE heroID=69;') -ne '9876' -or
        (Invoke-TestSql $preservedColumns) -cne $beforeMigration -or
        (Invoke-TestSql 'SELECT COUNT(*) FROM herouse WHERE heroID=60000 AND permissionID IS NULL;') -ne '1') {
        throw 'Repeated migration overwrote an existing permission, name, statistic, or unknown default.'
    }
    Write-Output 'PASS: migration SQL executed twice; 246 exact mappings, 68 free, names/statistics preserved, configured override retained.'
    [void](Invoke-TestSql 'DROP TABLE herouse;')
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
    if ($test.ExitCode -ne 0) { throw "Hero availability database tests failed with exit code $($test.ExitCode)." }
} finally {
    # Only stop the exact child process created here; never find/stop a service
    # by executable name, port, a PID file, or another process's identity.
    if ($null -ne $server -and !$server.HasExited) { $server.Kill(); $server.WaitForExit() }
    Write-Output ('Retained isolated test artifacts: ' + $temporaryRoot)
}
