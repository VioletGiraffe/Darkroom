# Builds Darkroom.pro: compiles and links the apps, runs nothing. Binaries land in bin\<configuration>.
# Exit code: 1 on a build failure, 2 when the environment is incomplete (see environment.ps1).
# Windows PowerShell 5.1 is enough; build.bat starts this script, the execution policy being what it is.

[CmdletBinding(PositionalBinding = $false, DefaultParameterSetName = 'Build')]
param(
	# Both apps when omitted
	[Parameter(Position = 0, ParameterSetName = 'Build')]
	[ValidateSet('darkroom', 'quickroom')]
	[string]$Target,

	# Rebuilds the whole solution from scratch: both apps and every library they link
	[Parameter(ParameterSetName = 'Rebuild')]
	[switch]$Rebuild,

	[ValidateSet('release', 'debug')]
	[string]$Configuration = 'release',

	# Overrides the toolset qmake writes into the projects; not older than the vcpkg triplet's (vcpkg-triplets/)
	[string]$PlatformToolset
)

. (Join-Path $PSScriptRoot 'environment.ps1')

# Project targets carry the subprojects' TARGET names; Rebuild is the solution-wide target
$msbuildTargets = if ($Rebuild) { @('Rebuild') } else
{
	switch ($Target)
	{
		'darkroom'  { @('Darkroom') }
		'quickroom' { @('Quickroom') }
		default     { @('Darkroom', 'Quickroom') }
	}
}

Install-VcpkgDependencies
Invoke-Qmake 'Darkroom.pro'
Invoke-MSBuild 'Darkroom.sln' $msbuildTargets $Configuration $PlatformToolset
exit 0
