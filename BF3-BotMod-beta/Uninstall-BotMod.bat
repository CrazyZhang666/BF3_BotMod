@echo off
:: Battl3Bots setup window, Uninstall selected - the work is done by files\BotMod-Setup.ps1
start "" powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0files\BotMod-Setup.ps1" -Mode uninstall %*
