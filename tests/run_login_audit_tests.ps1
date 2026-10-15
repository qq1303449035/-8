#requires -Version 7.0
# Verifies separate 0x5BDE UID/name event rows and existing ordinary login logs.
param(
    [Parameter(Mandatory = $true)][int]$Port,
    [Parameter(Mandatory = $true)][string]$User,
    [string]$Schema = ('chh8_test_media_login_' + [guid]::NewGuid().ToString('N')),
    [string]$MysqlRoot = 'D:\Program Files\mysql-8.0.39-winx64',
    [string]$VcVarsPath = 'D:\C++\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat'
)
$ErrorActionPreference = 'Stop'
if ($Port -le 0 -or $Port -gt 65535 -or $Port -eq 3306 -or [string]::IsNullOrEmpty($User) -or
    $Schema -notmatch '^chh8_test_media_login_[A-Za-z0-9_]+$' -or $Schema.Length -gt 64) {
    throw 'Only explicit nondefault loopback port, user and synthetic schema are allowed.'
}
$source = Join-Path $PSScriptRoot 'login_audit_tests.cpp'
$build = Join-Path ([IO.Path]::GetTempPath()) ('chh8-login-audit-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $build | Out-Null
$executable = Join-Path $build 'login_audit_tests.exe'
$object = Join-Path $build 'login_audit_tests.obj'
foreach ($path in @($source, $build, $MysqlRoot, $VcVarsPath)) {
    if ($path -match '["\r\n%!]') { throw 'Unsupported batch expansion characters in build paths.' }
}
if (!(Test-Path -LiteralPath $VcVarsPath -PathType Leaf)) { throw 'Visual Studio setup not found.' }
$compile = [Diagnostics.ProcessStartInfo]::new()
$compile.FileName = $env:ComSpec
$compile.UseShellExecute = $false
$compile.CreateNoWindow = $true
$compile.RedirectStandardOutput = $true
$compile.RedirectStandardError = $true
$compile.Arguments = '/d /c ' + ('call "{0}" >nul && cl /nologo /std:c++17 /utf-8 /EHsc /W4 /WX "{1}" /I"{2}\include" /Fo"{3}" /Fe"{4}" /link /LIBPATH:"{2}\lib"' -f
    $VcVarsPath, $source, $MysqlRoot, $object, $executable)
$compiler = [Diagnostics.Process]::Start($compile)
$compilerOutput = $compiler.StandardOutput.ReadToEndAsync()
$compilerError = $compiler.StandardError.ReadToEndAsync()
$compiler.WaitForExit()
Write-Output $compilerOutput.Result
Write-Output $compilerError.Result
if ($compiler.ExitCode -ne 0) { throw 'Login audit test compilation failed.' }

# Create only this new test schema. No service is started/stopped by this script.
$create = [Diagnostics.ProcessStartInfo]::new()
$create.FileName = Join-Path $MysqlRoot 'bin\mysql.exe'
$create.UseShellExecute = $false
$create.CreateNoWindow = $true
$create.RedirectStandardOutput = $true
$create.RedirectStandardError = $true
foreach ($argument in @('--no-defaults', '--protocol=TCP', '--host=127.0.0.1', "--port=$Port", "--user=$User", '--password=',
    '--execute', ('CREATE DATABASE `{0}` CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci' -f $Schema))) {
    $create.ArgumentList.Add($argument)
}
$database = [Diagnostics.Process]::Start($create)
$discardOutput = $database.StandardOutput.ReadToEnd()
$discardError = $database.StandardError.ReadToEnd()
$database.WaitForExit()
if ($database.ExitCode -ne 0) { throw 'Could not create the isolated test schema; raw diagnostics omitted.' }

$run = [Diagnostics.ProcessStartInfo]::new()
$run.FileName = $executable
$run.UseShellExecute = $false
$run.CreateNoWindow = $true
$run.RedirectStandardOutput = $true
$run.RedirectStandardError = $true
$run.Environment['CHH_DB_HOST'] = '127.0.0.1'
$run.Environment['CHH_DB_PORT'] = [string]$Port
$run.Environment['CHH_DB_USER'] = $User
$run.Environment['CHH_DB_NAME'] = $Schema
# The empty value must be present; _putenv_s would delete it on Windows.
$run.Environment['CHH_DB_PASSWORD'] = ''
$run.Environment['PATH'] = (Join-Path $MysqlRoot 'lib') + ';' + (Join-Path $MysqlRoot 'bin') + ';' + $env:PATH
$test = [Diagnostics.Process]::Start($run)
$testOutput = $test.StandardOutput.ReadToEndAsync()
$testError = $test.StandardError.ReadToEndAsync()
$test.WaitForExit()
Write-Output $testOutput.Result
Write-Output $testError.Result
Write-Output ('Test artifacts: ' + $build)
Write-Output ('Isolated schema retained: ' + $Schema)
if ($test.ExitCode -ne 0) { throw "Login audit tests failed with exit code $($test.ExitCode)." }
