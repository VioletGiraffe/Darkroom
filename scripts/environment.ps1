# Dot-sourced by build.ps1 and run_tests.ps1. Puts the Qt kit and the MSVC environment in place and sets:
#   $repositoryRoot - the Darkroom checkout
#   $qmake          - the kit's qmake.exe
#   $vcpkg          - vcpkg.exe
# Exits with code 2 when the environment is incomplete.
#
# The Qt kit comes from QT_ROOT_DIR, or from a git-ignored local-env.ps1 beside this script that sets it.
# vcpkg comes from VCPKG_ROOT, else from PATH.
# MSBuild and qmake need the MSVC environment: qmake probes cl for the compiler version and writes the matching toolset
# into the projects it generates.

function Fail-Environment([string]$message)
{
	[Console]::Error.WriteLine($message)
	exit 2
}

$repositoryRoot = Split-Path $PSScriptRoot -Parent

if (-not $env:QT_ROOT_DIR)
{
	$localEnvironment = Join-Path $PSScriptRoot 'local-env.ps1'
	if (Test-Path $localEnvironment) { . $localEnvironment }
}
if (-not $env:QT_ROOT_DIR)
{
	Fail-Environment "QT_ROOT_DIR is not set: set it to the Qt kit directory, the one holding bin\qmake.exe, or set it in a git-ignored local-env.ps1 beside this script."
}
$qmake = Join-Path $env:QT_ROOT_DIR 'bin\qmake.exe'
if (-not (Test-Path $qmake)) { Fail-Environment "No qmake.exe under `"$env:QT_ROOT_DIR\bin`"." }
# The test executable needs the Qt DLLs
$env:PATH = (Join-Path $env:QT_ROOT_DIR 'bin') + ';' + $env:PATH

# Before the MSVC environment, which can set VCPKG_ROOT to Visual Studio's own vcpkg
$vcpkg = if ($env:VCPKG_ROOT) { Join-Path $env:VCPKG_ROOT 'vcpkg.exe' } else { (Get-Command vcpkg -ErrorAction SilentlyContinue).Source }
if (-not $vcpkg -or -not (Test-Path $vcpkg)) { Fail-Environment 'vcpkg was not found: set VCPKG_ROOT to its directory or put it on PATH.' }

if (-not (Get-Command cl -ErrorAction SilentlyContinue))
{
	$installerDirectory = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
	$vswhere = Join-Path $installerDirectory 'vswhere.exe'
	if (-not (Test-Path $vswhere)) { Fail-Environment 'vswhere.exe was not found. Install Visual Studio with the C++ toolset.' }

	$visualStudio = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath |
		Select-Object -First 1
	if (-not $visualStudio) { Fail-Environment 'No Visual Studio with the C++ toolset was found.' }

	# vcvars64.bat itself calls a bare vswhere.exe, which it expects on PATH
	$env:PATH += ';' + $installerDirectory
	# A child process's environment is not inherited back, so vcvars64.bat's is read out of cmd and imported
	$vcvars = Join-Path $visualStudio 'VC\Auxiliary\Build\vcvars64.bat'
	cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
		if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
	}
}

# Installs vcpkg.json's dependencies for the triplet vcpkg.pri links against. Exits with code 1 when the install fails.
function Install-VcpkgDependencies
{
	& $vcpkg install --triplet x64-windows-static-md "--x-manifest-root=$repositoryRoot"
	if ($LASTEXITCODE -ne 0) { exit 1 }
}

# Runs qmake on a top-level project in the repository root. Exits with code 1 when qmake fails.
function Invoke-Qmake([string]$project)
{
	# A kept .qmake.stash pins the toolchain probed when it was written, so every run probes the current one instead.
	# One is written per subproject build directory, and qmake also searches upward, so the sweep covers the repository.
	Get-ChildItem $repositoryRoot -Recurse -Force -Filter '.qmake.stash' -ErrorAction SilentlyContinue | Remove-Item -Force

	Push-Location $repositoryRoot
	try
	{
		& $qmake -v
		& $qmake -tp vc -r $project
		if ($LASTEXITCODE -ne 0) { exit 1 }
	}
	finally { Pop-Location }
}

# Builds the given targets of a solution in the repository root: project names, or a solution target such as Rebuild.
# Exits with code 1 on a build failure.
# platformToolset overrides the toolset qmake wrote into the projects; empty keeps it.
function Invoke-MSBuild([string]$solution, [string[]]$targets, [string]$configuration, [string]$platformToolset)
{
	$properties = "/p:Configuration=$configuration"
	if ($platformToolset) { $properties += ";PlatformToolset=$platformToolset" }
	# msbuild, not nmake: it also rebuilds what the compiler command line changed for, such as the Qt include paths
	msbuild (Join-Path $repositoryRoot $solution) "/t:$($targets -join ';')" /nologo /m /v:minimal /clp:Summary $properties
	if ($LASTEXITCODE -ne 0) { exit 1 }
}
