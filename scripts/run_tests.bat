@echo off
:: Starts run_tests.ps1, which holds the logic. A .ps1 cannot be run directly under the default execution policy.
:: Windows PowerShell, not pwsh: it is present on every machine.
:: Usage: run_tests.bat [-Configuration debug] [-BuildOnly or -NoBuild] [-PlatformToolset v143] [Catch2 arguments]
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_tests.ps1" %*
