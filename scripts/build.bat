@echo off
:: Starts build.ps1, which holds the logic. A .ps1 cannot be run directly under the default execution policy.
:: Windows PowerShell, not pwsh: it is present on every machine.
:: Usage: build.bat [darkroom, quickroom or -Rebuild] [-Configuration debug] [-PlatformToolset v143]; no target builds both apps.
:: -Rebuild rebuilds the whole solution from scratch.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
