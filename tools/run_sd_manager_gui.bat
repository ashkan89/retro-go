@echo off
setlocal

set "PYTHON=%LOCALAPPDATA%\Programs\Python\Python311\python.exe"
if not exist "%PYTHON%" set "PYTHON=python"

cd /d "%~dp0\.."
"%PYTHON%" "tools\retro_go_sd_backup.py" %*
