@echo off
rem Drop the Magnific WAV downloads into the "raw" folder beside this file, then run this.
rem Results land in G:\Grand Theft Auto 2\gta2dx9_voice, named by text key, ready for the game.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0phone_batch.ps1"
pause
