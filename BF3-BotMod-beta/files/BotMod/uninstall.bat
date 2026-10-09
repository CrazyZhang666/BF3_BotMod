@echo off
:: Restores the original xinput9_1_0.dll and removes the BotMod core (server must be stopped).
setlocal
set "SRV=%~dp0..\Battlefield_3_Server"
del /q "%SRV%\BotModCore.dll" "%SRV%\BotModCore.loaded*.dll" 2>nul
if not exist "%SRV%\xinput9_1_0_orig.dll" ( echo BotMod is not installed. & exit /b 0 )
move /y "%SRV%\xinput9_1_0_orig.dll" "%SRV%\xinput9_1_0.dll" >nul
echo BotMod removed.
