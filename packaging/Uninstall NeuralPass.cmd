@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Uninstall-NeuralPass.ps1" %*
exit /b %errorlevel%
