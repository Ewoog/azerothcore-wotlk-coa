@echo off
rem Starts CoAPatcher.ps1 (Windows PowerShell 5.1, single-threaded apartment for drag and drop).
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0CoAPatcher.ps1"
if errorlevel 1 pause
