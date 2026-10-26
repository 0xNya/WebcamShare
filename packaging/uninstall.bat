@echo off
rem Stops WebcamShare, disables autostart and removes the virtual camera
rem (asks for admin rights once). Settings and log stay in %APPDATA%\WebcamShare.
"%~dp0WebcamShare.exe" --uninstall
