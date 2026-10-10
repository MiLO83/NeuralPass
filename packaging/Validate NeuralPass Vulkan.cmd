@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Validate-NeuralPassVulkan.ps1" %*
exit /b %errorlevel%
