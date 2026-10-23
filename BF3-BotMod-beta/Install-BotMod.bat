@echo off
:: Battl3Bots setup window (install / update / uninstall) - the work is done by files\BotMod-Setup.ps1
start "" powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0files\BotMod-Setup.ps1" -Mode install %*
