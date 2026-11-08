@echo off
cd /d "%~dp0"
set PY=C:\Python311\python.exe
if not exist "%PY%" set PY=python
"%PY%" at_assistant.py
if errorlevel 1 pause
