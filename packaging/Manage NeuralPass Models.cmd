@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Manage-NeuralPassModels.ps1" %*
exit /b %errorlevel%
