@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Validate-NeuralPassWorker.ps1" %*
exit /b %errorlevel%
