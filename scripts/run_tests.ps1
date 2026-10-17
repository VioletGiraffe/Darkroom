# Builds Tests.pro and runs DarkroomTests. Exit code: 1 on a build or test failure, 2 when the environment is incomplete
# (see environment.ps1).
# Windows PowerShell 5.1 is enough; run_tests.bat starts this script, the execution policy being what it is.

# No positional binding: every parameter is named, so that a Catch2 spec cannot bind to one by position
[CmdletBinding(PositionalBinding = $false)]
param(
	[ValidateSet('release', 'debug')]
	[string]$Configuration = 'release',

	# -BuildOnly builds without running; -NoBuild runs what is already built
	[switch]$BuildOnly,
	[switch]$NoBuild,

	# Overrides the toolset qmake writes into the projects, e.g. v143 to build with an older one than the newest installed
	[string]$PlatformToolset,

	# Arguments for DarkroomTests, e.g. a Catch2 test spec
	[Parameter(ValueFromRemainingArguments)]
	[string[]]$TestArguments
)

. (Join-Path $PSScriptRoot 'environment.ps1')

if (-not $NoBuild)
{
	Invoke-Qmake 'Tests.pro'
	Invoke-MSBuild 'Tests.sln' @('DarkroomTests') $Configuration $PlatformToolset
}
if ($BuildOnly) { exit 0 }

$executable = Join-Path $repositoryRoot "bin\$Configuration\DarkroomTests.exe"
if (-not (Test-Path $executable))
{
	Write-Host 'DarkroomTests: not built'
	exit 1
}

# --warn NoTests: a filter that matches nothing exits 0 otherwise, so a broken one would pass silently
& $executable @TestArguments --warn NoTests
if ($LASTEXITCODE -ne 0) { exit 1 }
exit 0
