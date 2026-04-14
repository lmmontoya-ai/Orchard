param(
  [string]$ServiceName = ("OrchardSmoke-" + [Guid]::NewGuid().ToString("N").Substring(0, 8)),
  [string]$DisplayName,
  [string]$TargetPath,
  [string]$MountPoint,
  [UInt64]$VolumeOid,
  [int]$MountTimeoutMs = 15000
)

$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $true
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot "OrchardWinFspTestCommon.ps1")

$repoRoot = Get-OrchardRepoRoot -ScriptRoot $PSScriptRoot
$serviceHostExecutable = Join-Path $repoRoot "build\\default\\src\\mount-service\\orchard-service-host.exe"
if (-not (Test-Path $serviceHostExecutable)) {
  throw "Missing orchard-service-host executable at '$serviceHostExecutable'. Build the repo first."
}

if (-not $DisplayName) {
  $DisplayName = $ServiceName
}
if (-not $TargetPath) {
  $TargetPath = Join-Path $repoRoot "tests\\corpus\\samples\\plain-user-data.img"
}
if (-not $MountPoint) {
  $freeDriveLetter = Get-OrchardFreeDriveLetter
  if (-not $freeDriveLetter) {
    throw "No free drive letter is available for the SCM smoke run."
  }
  $MountPoint = "$freeDriveLetter`:"
}

$resolvedTargetPath = (Resolve-Path $TargetPath).Path

function Wait-OrchardServiceState {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Name,

    [Parameter(Mandatory = $true)]
    [string]$ExpectedStatus,

    [int]$TimeoutMs = 15000
  )

  $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
  do {
    $service = Get-Service -Name $Name -ErrorAction Stop
    if ($service.Status.ToString() -eq $ExpectedStatus) {
      return
    }
    Start-Sleep -Milliseconds 250
  } while ($stopwatch.ElapsedMilliseconds -lt $TimeoutMs)

  throw "Timed out waiting for service '$Name' to reach state '$ExpectedStatus'."
}

function Wait-OrchardMountPoint {
  param(
    [Parameter(Mandatory = $true)]
    [string]$MountPoint,

    [Parameter(Mandatory = $true)]
    [bool]$Present,

    [int]$TimeoutMs = 15000
  )

  $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
  do {
    $exists = Test-Path "$MountPoint\\"
    if ($exists -eq $Present) {
      return
    }
    Start-Sleep -Milliseconds 250
  } while ($stopwatch.ElapsedMilliseconds -lt $TimeoutMs)

  $expected = if ($Present) { "appear" } else { "disappear" }
  throw "Timed out waiting for mount point '$MountPoint' to $expected."
}

$installed = $false
try {
  $installArgs = @(
    "--install",
    "--service-name", $ServiceName,
    "--display-name", $DisplayName,
    "--target", $resolvedTargetPath,
    "--mountpoint", $MountPoint
  )
  if ($PSBoundParameters.ContainsKey("VolumeOid")) {
    $installArgs += @("--volume-oid", $VolumeOid.ToString())
  }

  & $serviceHostExecutable @installArgs
  $installed = $true

  Start-Service -Name $ServiceName
  Wait-OrchardServiceState -Name $ServiceName -ExpectedStatus Running
  Wait-OrchardMountPoint -MountPoint $MountPoint -Present $true -TimeoutMs $MountTimeoutMs

  $readChecks = Invoke-OrchardPlainReadChecks -MountPoint $MountPoint

  Stop-Service -Name $ServiceName
  Wait-OrchardServiceState -Name $ServiceName -ExpectedStatus Stopped
  Wait-OrchardMountPoint -MountPoint $MountPoint -Present $false -TimeoutMs $MountTimeoutMs

  [pscustomobject]@{
    service_name = $ServiceName
    display_name = $DisplayName
    target_path = $resolvedTargetPath
    mount_point = $MountPoint
    read_checks = $readChecks
    status = "stopped_after_smoke"
  } | ConvertTo-Json -Depth 3
}
finally {
  if ($installed) {
    try {
      & $serviceHostExecutable --uninstall --service-name $ServiceName | Out-Null
    }
    catch {
      Write-Warning "Failed to uninstall smoke service '$ServiceName': $($_.Exception.Message)"
    }
  }
}
