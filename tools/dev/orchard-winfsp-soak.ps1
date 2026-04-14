param(
  [int]$Cycles = 12,
  [int]$HoldMs = 60000,
  [int]$MountTimeoutMs = 15000,
  [int]$UnmountTimeoutMs = 10000,
  [int]$WarmupCycles = 2
)

$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $true
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot "OrchardWinFspTestCommon.ps1")

$repoRoot = Get-OrchardRepoRoot -ScriptRoot $PSScriptRoot
$scenarios = @(
  [pscustomobject]@{
    name = "plain-user-data"
    target_path = Join-Path $repoRoot "tests\\corpus\\samples\\plain-user-data.img"
    check = {
      param([string]$MountPoint)
      Invoke-OrchardPlainReadChecks -MountPoint $MountPoint
    }
  },
  [pscustomobject]@{
    name = "explorer-large"
    target_path = Join-Path $repoRoot "tests\\corpus\\samples\\explorer-large.img"
    check = {
      param([string]$MountPoint)
      Invoke-OrchardExplorerStressChecks -MountPoint $MountPoint
    }
  },
  [pscustomobject]@{
    name = "link-behavior"
    target_path = Join-Path $repoRoot "tests\\corpus\\samples\\link-behavior.img"
    check = {
      param([string]$MountPoint)
      Invoke-OrchardLinkBehaviorChecks -MountPoint $MountPoint
    }
  }
)

if ($Cycles -lt 1) {
  throw "Cycles must be at least 1."
}
if ($WarmupCycles -lt 0) {
  throw "WarmupCycles must be non-negative."
}

$results = New-Object System.Collections.Generic.List[object]

for ($cycle = 1; $cycle -le $Cycles; ++$cycle) {
  foreach ($scenario in $scenarios) {
    $session = Start-OrchardMountSmokeSession `
      -RepoRoot $repoRoot `
      -TargetPath $scenario.target_path `
      -HoldMs $HoldMs `
      -MountTimeoutMs $MountTimeoutMs

    try {
      $telemetry = Get-OrchardMountProcessTelemetry -Session $session
      $checkResult = & $scenario.check $session.mount_point
    }
    finally {
      $stopResult = Stop-OrchardMountSmokeSession -Session $session -UnmountTimeoutMs $UnmountTimeoutMs
    }

    $results.Add([pscustomobject]@{
      cycle = $cycle
      scenario = $scenario.name
      mount_point = $session.mount_point
      mount_elapsed_ms = $session.mount_elapsed_ms
      unmount_elapsed_ms = $stopResult.unmount_elapsed_ms
      exit_code = $stopResult.exit_code
      mount_point_removed = $stopResult.mount_point_removed
      handle_count = $telemetry.handle_count
      working_set_bytes = $telemetry.working_set_bytes
      private_bytes = $telemetry.private_bytes
      check_result = $checkResult
    })
  }
}

$warmupFloor = [Math]::Min($WarmupCycles, $Cycles - 1)
$plateauResults = @($results | Where-Object { $_.cycle -gt $warmupFloor })
if (-not $plateauResults) {
  $plateauResults = @($results)
}

foreach ($scenarioName in ($scenarios | Select-Object -ExpandProperty name)) {
  $scenarioResults = @($plateauResults | Where-Object { $_.scenario -eq $scenarioName })
  if (-not $scenarioResults) {
    continue
  }

  $mountTimes = @($scenarioResults | Select-Object -ExpandProperty mount_elapsed_ms)
  $unmountTimes = @($scenarioResults | Select-Object -ExpandProperty unmount_elapsed_ms)
  $handleCounts = @($scenarioResults | Select-Object -ExpandProperty handle_count | Where-Object { $_ -ne $null })
  $privateBytes = @($scenarioResults | Select-Object -ExpandProperty private_bytes | Where-Object { $_ -ne $null })

  if (($mountTimes | Measure-Object -Maximum).Maximum -gt ($MountTimeoutMs * 0.9)) {
    throw "Mount times approached the timeout threshold for scenario '$scenarioName'."
  }
  if (($unmountTimes | Measure-Object -Maximum).Maximum -gt ($UnmountTimeoutMs * 0.9)) {
    throw "Unmount times approached the timeout threshold for scenario '$scenarioName'."
  }

  if ($handleCounts.Count -ge 2) {
    $handleBaseline = [double]$handleCounts[0]
    $handlePeak = [double](($handleCounts | Measure-Object -Maximum).Maximum)
    if ($handlePeak -gt [Math]::Max($handleBaseline + 16.0, $handleBaseline * 1.5)) {
      throw "Handle-count telemetry drifted upward for scenario '$scenarioName'."
    }
  }

  if ($privateBytes.Count -ge 2) {
    $privateBaseline = [double]$privateBytes[0]
    $privatePeak = [double](($privateBytes | Measure-Object -Maximum).Maximum)
    if ($privatePeak -gt [Math]::Max($privateBaseline + 20MB, $privateBaseline * 1.75)) {
      throw "Private-byte telemetry drifted upward for scenario '$scenarioName'."
    }
  }
}

[pscustomobject]@{
  cycles = $Cycles
  warmup_cycles = $WarmupCycles
  scenarios = @(
    foreach ($scenarioName in ($scenarios | Select-Object -ExpandProperty name)) {
      $scenarioResults = @($results | Where-Object { $_.scenario -eq $scenarioName })
      [pscustomobject]@{
        name = $scenarioName
        runs = $scenarioResults.Count
        max_mount_elapsed_ms = ($scenarioResults | Measure-Object -Property mount_elapsed_ms -Maximum).Maximum
        max_unmount_elapsed_ms = ($scenarioResults | Measure-Object -Property unmount_elapsed_ms -Maximum).Maximum
        max_handle_count = ($scenarioResults | Measure-Object -Property handle_count -Maximum).Maximum
        max_private_bytes = ($scenarioResults | Measure-Object -Property private_bytes -Maximum).Maximum
      }
    }
  )
} | ConvertTo-Json -Depth 5
