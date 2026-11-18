@echo off
rem Open a PR for the current branch: pr.bat  (uses commits as title/body)  or  pr.bat "Title" "Body"
cd /d "%~dp0..\.."
if "%~1"=="" (python tools\automation\ship.py pr) else python tools\automation\ship.py pr -t %1 -b %2
