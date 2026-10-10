@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Update-NeuralPass.ps1" %*
exit /b %errorlevel%
