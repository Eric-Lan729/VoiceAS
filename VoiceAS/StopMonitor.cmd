@echo off
"%~dp0VoiceAS.exe" --stop
if errorlevel 1 (
    echo Press any key to close this window...
    pause >nul
)
