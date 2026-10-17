@echo off
"%~dp0VoiceAS.exe" --monitor %*
if errorlevel 1 pause
