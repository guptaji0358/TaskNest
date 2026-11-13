@echo off
rem Commit STAGED changes and push the current branch: push.bat "commit message"
cd /d "%~dp0..\.."
python tools\automation\ship.py push -m %*
