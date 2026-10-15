@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"
set "missing=0"
for %%F in ("茶会服务器8.exe" "libmysql.dll" "libssl-3-x64.dll" "libcrypto-3-x64.dll" "server.pfx" "msvcp140.dll" "vcruntime140.dll" "vcruntime140_1.dll") do (
    if not exist "%%~F" (
        echo [启动检查] 缺少必需文件：%%~F
        set "missing=1"
    )
)
if "%missing%"=="1" (
    echo 请复制完整服务器运行包，并将本脚本与上述文件放在同一目录。
    echo 当前目录："%CD%"
    pause
    exit /b 2
)
echo 正在启动茶会服务器8；此窗口将保留启动错误和退出码。
".\茶会服务器8.exe" --no-pause %*
set "server_exit=%errorlevel%"
echo.
echo 服务器退出码：%server_exit%
echo 启动日志："%CD%\server-startup.log"
echo 若日志不存在，程序可能在 Windows 加载 DLL 阶段失败，或当前目录不可写。
pause
exit /b %server_exit%
