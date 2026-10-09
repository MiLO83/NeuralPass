@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Diagnose-NeuralPass.ps1" %*
exit /b %errorlevel%
