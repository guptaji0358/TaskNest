@echo off
rem Read-only review of the working tree: review.bat [--diff] [--expect PATH ...]
cd /d "%~dp0..\.."
python tools\automation\review.py %*
