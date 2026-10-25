# Runs image_read_benchmark.py on a dedicated VHDX volume, remounting it before every sample to discard its cache.
# Needs an elevated prompt: creating, mounting and dismounting a virtual disk is administrative.
# What to verify before trusting the numbers: docs/build.md, "Image read benchmark".

[CmdletBinding(PositionalBinding = $false)]
param(
	# The VHDX file, placed on the disk to measure
	[Parameter(Mandatory)]
	[string]$VhdxPath,

	# Creates the VHDX if missing and generates the benchmark files on it
	[switch]$Setup,

	# Rounds: every round times each variant on each layout once
	[int]$Samples = 0,

	# Per-sample CSV, appended to
	[string]$Out,

	# Remounts the VHDX read-only and prints its root: the benchmark runs this before every sample
	[switch]$Remount,

	# Passed on to the benchmark, e.g. --variants buffered,overlapped-q8 or --batch 60
	[Parameter(ValueFromRemainingArguments)]
	[string[]]$BenchmarkArguments = @()
)

$ErrorActionPreference = 'Stop'

function Fail([string]$message)
{
	[Console]::Error.WriteLine($message)
	exit 2
}

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { Fail 'Run this from an elevated prompt.' }
if (-not $Remount -and -not $Setup -and $Samples -le 0) { Fail 'Nothing to do: pass -Setup, -Samples N, or both.' }
if ($Samples -gt 0 -and -not $Out) { Fail '-Samples needs -Out.' }

# diskpart and the Storage cmdlets both need absolute paths
$VhdxPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($VhdxPath)

# The letter can change from one mount to the next, and appears shortly after the mount returns
function Get-VolumeRoot([string]$vhdx)
{
	for ($attempt = 0; $attempt -lt 50; ++$attempt)
	{
		$partition = Get-DiskImage -ImagePath $vhdx | Get-Disk | Get-Partition | Where-Object { [int][char]$_.DriveLetter -ne 0 } | Select-Object -First 1
		if ($partition) { return "$($partition.DriveLetter):\" }
		Start-Sleep -Milliseconds 100
	}
	Fail "$vhdx has no volume with a drive letter"
}

function Mount-Vhdx([string]$vhdx, [string]$access)
{
	if (-not (Get-DiskImage -ImagePath $vhdx).Attached) { Mount-DiskImage -ImagePath $vhdx -Access $access | Out-Null }
	return Get-VolumeRoot $vhdx
}

function Dismount-Vhdx([string]$vhdx)
{
	if ((Get-DiskImage -ImagePath $vhdx).Attached) { Dismount-DiskImage -ImagePath $vhdx | Out-Null }
}

function New-BenchmarkVhdx([string]$vhdx)
{
	New-Item -ItemType Directory -Force -Path (Split-Path $vhdx -Parent) | Out-Null
	$diskpartScript = [IO.Path]::GetTempFileName()
	try
	{
		# Fixed size: an expanding file grows wherever the host has room, and seeks within it would be measured too
		Set-Content -Path $diskpartScript -Encoding Ascii -Value @(
			# Room for the default generation: two layouts of 2400 files and the filler, about 2.3 GiB
			"create vdisk file=`"$vhdx`" maximum=3072 type=fixed"
			"select vdisk file=`"$vhdx`""
			'attach vdisk'
			'create partition primary'
			'format fs=ntfs quick label=ImageReadBench'
			'assign'
		)
		$output = diskpart /s $diskpartScript
		if ($LASTEXITCODE -ne 0) { Fail "diskpart failed to create $vhdx`n$($output -join "`n")" }
	}
	finally { Remove-Item $diskpartScript }
}

if ($Remount)
{
	# Dismounting discards everything cached for the volume; read-only, so that reading writes nothing back
	Dismount-Vhdx $VhdxPath
	Mount-Vhdx $VhdxPath 'ReadOnly'
	exit 0
}

$benchmark = Join-Path $PSScriptRoot 'image_read_benchmark.py'

if ($Setup)
{
	if (-not (Test-Path $VhdxPath)) { New-BenchmarkVhdx $VhdxPath }

	$root = Mount-Vhdx $VhdxPath 'ReadWrite'
	# Windows Search reading the files after a mount would warm the cache before the sample
	$volume = Get-CimInstance Win32_Volume -Filter "DriveLetter='$($root.Substring(0, 2))'"
	Set-CimInstance -InputObject $volume -Property @{ IndexingEnabled = $false }

	# Layouts that already exist are kept, so an interrupted setup can simply be rerun
	& python $benchmark generate $root
	if ($LASTEXITCODE -ne 0) { Fail "Generating the benchmark files on $VhdxPath failed" }

	Dismount-Vhdx $VhdxPath
}

if ($Samples -gt 0)
{
	if (-not (Test-Path $VhdxPath)) { Fail "$VhdxPath does not exist: -Setup creates it." }

	# The same PowerShell runs the remounts
	$shell = (Get-Process -Id $PID).Path
	$remountCommand = @($shell, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath, '-Remount', '-VhdxPath', $VhdxPath)

	# One --remount per argument: Windows PowerShell mangles the embedded quotes a single command line would need
	$remountArguments = @($remountCommand | ForEach-Object { "--remount=$_" })
	$root = Mount-Vhdx $VhdxPath 'ReadOnly'
	& python $benchmark run $root --samples $Samples --out $Out @remountArguments @BenchmarkArguments
	if ($LASTEXITCODE -ne 0) { Fail "The benchmark failed on $VhdxPath" }

	Dismount-Vhdx $VhdxPath
}
