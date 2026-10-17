# Builds Darkroom.pro: compiles and links the apps, runs nothing. Binaries land in bin\<configuration>.
# Exit code: 1 on a build failure, 2 when the environment is incomplete (see environment.ps1).
# Windows PowerShell 5.1 is enough; build.bat starts this script, the execution policy being what it is.

[CmdletBinding(PositionalBinding = $false)]
param(
	# Both apps when omitted
	[Parameter(Position = 0)]
	[ValidateSet('darkroom', 'quickroom')]
	[string]$Target,

	[ValidateSet('release', 'debug')]
	[string]$Configuration = 'release',

	# Overrides the toolset qmake writes into the projects, e.g. v143 to build with an older one than the newest installed
	[string]$PlatformToolset
)

. (Join-Path $PSScriptRoot 'environment.ps1')

# The msbuild project names are the targets' TARGET names
$projects = switch ($Target)
{
	'darkroom'  { @('Darkroom') }
	'quickroom' { @('Quickroom') }
	default     { @('Darkroom', 'Quickroom') }
}

Invoke-Qmake 'Darkroom.pro'
Invoke-MSBuild 'Darkroom.sln' $projects $Configuration $PlatformToolset
exit 0
