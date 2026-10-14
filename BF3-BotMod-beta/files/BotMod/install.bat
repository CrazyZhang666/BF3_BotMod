@echo off
:: Installs BotMod into the BF3 server folder. The original xinput9_1_0.dll is kept as xinput9_1_0_orig.dll.
:: Pass "core" to update only BotModCore.dll (safe while the server runs; then send the "reload" command).
setlocal
set "SRV=%~dp0..\Battlefield_3_Server"
if not exist "%~dp0out\BotModCore.dll" ( echo Run build.bat first. & exit /b 1 )
copy /y "%~dp0out\BotModCore.dll" "%SRV%\BotModCore.dll" >nul
if /i "%~1"=="core" ( echo BotMod core updated - send "reload". & exit /b 0 )
if not exist "%SRV%\xinput9_1_0_orig.dll" copy /y "%SRV%\xinput9_1_0.dll" "%SRV%\xinput9_1_0_orig.dll" >nul
copy /y "%~dp0out\xinput9_1_0.dll" "%SRV%\xinput9_1_0.dll" >nul
echo BotMod installed.
