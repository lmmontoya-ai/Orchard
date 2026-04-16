Set-StrictMode -Version Latest

function Get-OrchardRepoRoot {
  param(
    [Parameter(Mandatory = $true)]
    [string]$ScriptRoot
  )

  return Split-Path -Parent (Split-Path -Parent $ScriptRoot)
}

function Get-OrchardFreeDriveLetter {
  param(
    [string[]]$CandidateLetters = @("R", "S", "T", "U", "V", "W", "X", "Y", "Z")
  )

  return $CandidateLetters |
    Where-Object { -not (Get-PSDrive -Name $_ -ErrorAction SilentlyContinue) } |
    Select-Object -First 1
}

function Resolve-OrchardMountSmokeExecutable {
  param(
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot
  )

  $mountSmokeExecutable = Join-Path $RepoRoot "build\\default\\tools\\mount-smoke\\orchard-mount-smoke.exe"
  if (-not (Test-Path $mountSmokeExecutable)) {
    throw "Missing orchard-mount-smoke executable at '$mountSmokeExecutable'. Build the repo first."
  }

  return $mountSmokeExecutable
}

function Start-OrchardMountSmokeSession {
  param(
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot,

    [Parameter(Mandatory = $true)]
    [string]$TargetPath,

    [string]$MountPoint,

    [int]$HoldMs = 60000,

    [int]$MountTimeoutMs = 15000
  )

  $resolvedTargetPath = (Resolve-Path $TargetPath).Path
  $mountSmokeExecutable = Resolve-OrchardMountSmokeExecutable -RepoRoot $RepoRoot

  if (-not $MountPoint) {
    $freeDriveLetter = Get-OrchardFreeDriveLetter
    if (-not $freeDriveLetter) {
      throw "No free drive letter is available for the WinFsp smoke run."
    }
    $MountPoint = "$freeDriveLetter`:"
  }

  $shutdownEventName = "Local\OrchardMountSmoke-" + [Guid]::NewGuid().ToString("N")
  $createdNew = $false
  $shutdownEvent =
    New-Object System.Threading.EventWaitHandle(
      $false,
      [System.Threading.EventResetMode]::ManualReset,
      $shutdownEventName,
      [ref]$createdNew)

  $mountStopwatch = [System.Diagnostics.Stopwatch]::StartNew()
  $mountProcess =
    Start-Process -FilePath $mountSmokeExecutable `
      -ArgumentList @(
        "--target", $resolvedTargetPath,
        "--mountpoint", $MountPoint,
        "--hold-ms", $HoldMs,
        "--shutdown-event", $shutdownEventName
      ) `
      -PassThru `
      -WindowStyle Hidden

  $mounted = $false
  $maxAttempts = [Math]::Max(1, [Math]::Ceiling($MountTimeoutMs / 250.0))
  for ($attempt = 0; $attempt -lt $maxAttempts; ++$attempt) {
    Start-Sleep -Milliseconds 250
    if (Test-Path "$MountPoint\\") {
      $mounted = $true
      break
    }

    if ($mountProcess.HasExited) {
      $shutdownEvent.Dispose()
      throw "orchard-mount-smoke exited early with code $($mountProcess.ExitCode)."
    }
  }

  if (-not $mounted) {
    $shutdownEvent.Set() | Out-Null
    $shutdownEvent.Dispose()
    throw "The mount point '$MountPoint' did not appear before timeout."
  }

  $mountStopwatch.Stop()

  return [pscustomobject]@{
    repo_root = $RepoRoot
    target_path = $resolvedTargetPath
    mount_point = $MountPoint
    mount_elapsed_ms = [int]$mountStopwatch.ElapsedMilliseconds
    shutdown_event = $shutdownEvent
    shutdown_event_name = $shutdownEventName
    mount_process = $mountProcess
  }
}

function Get-OrchardMountProcessTelemetry {
  param(
    [Parameter(Mandatory = $true)]
    [pscustomobject]$Session
  )

  $process = Get-Process -Id $Session.mount_process.Id -ErrorAction SilentlyContinue
  if (-not $process) {
    return [pscustomobject]@{
      id = $Session.mount_process.Id
      has_exited = $true
      handle_count = $null
      working_set_bytes = $null
      private_bytes = $null
    }
  }

  return [pscustomobject]@{
    id = $process.Id
    has_exited = $false
    handle_count = $process.HandleCount
    working_set_bytes = $process.WorkingSet64
    private_bytes = $process.PrivateMemorySize64
  }
}

function Stop-OrchardMountSmokeSession {
  param(
    [Parameter(Mandatory = $true)]
    [pscustomobject]$Session,

    [int]$UnmountTimeoutMs = 10000
  )

  $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
  try {
    if ($Session.shutdown_event) {
      $Session.shutdown_event.Set() | Out-Null
    }

    $process = $Session.mount_process
    $maxAttempts = [Math]::Max(1, [Math]::Ceiling($UnmountTimeoutMs / 250.0))
    for ($attempt = 0; $attempt -lt $maxAttempts; ++$attempt) {
      if ($process.HasExited) {
        break
      }
      Start-Sleep -Milliseconds 250
      $process.Refresh()
    }

    if (-not $process.HasExited) {
      throw "orchard-mount-smoke did not exit before the unmount timeout."
    }

    for ($attempt = 0; $attempt -lt $maxAttempts; ++$attempt) {
      if (-not (Test-Path "$($Session.mount_point)\\")) {
        break
      }
      Start-Sleep -Milliseconds 250
    }

    if (Test-Path "$($Session.mount_point)\\") {
      throw "Mount point '$($Session.mount_point)' still exists after orchard-mount-smoke exited."
    }

    $stopwatch.Stop()
    return [pscustomobject]@{
      exit_code = $process.ExitCode
      unmount_elapsed_ms = [int]$stopwatch.ElapsedMilliseconds
      mount_point_removed = $true
    }
  }
  finally {
    if ($Session.shutdown_event) {
      $Session.shutdown_event.Dispose()
    }
  }
}

function Invoke-OrchardPlainReadChecks {
  param(
    [Parameter(Mandatory = $true)]
    [string]$MountPoint,

    [string]$RootFile = "alpha.txt",
    [string]$NestedFile = "docs\\note.txt"
  )

  $rootEntries = Get-ChildItem -LiteralPath "$MountPoint\\" | Select-Object -ExpandProperty Name
  $alphaText = [System.IO.File]::ReadAllText((Join-Path $MountPoint $RootFile))
  $noteText = [System.IO.File]::ReadAllText((Join-Path $MountPoint $NestedFile))

  if ($alphaText -ne "Hello Orchard`n") {
    throw "Plain-read smoke returned unexpected alpha.txt contents."
  }
  if ($noteText -ne "Nested note`n") {
    throw "Plain-read smoke returned unexpected docs\\note.txt contents."
  }

  return [pscustomobject]@{
    root_entry_count = $rootEntries.Count
    alpha_text = $alphaText.TrimEnd("`r", "`n")
    note_text = $noteText.TrimEnd("`r", "`n")
  }
}

function Invoke-OrchardExplorerStressChecks {
  param(
    [Parameter(Mandatory = $true)]
    [string]$MountPoint,

    [string]$LargeDirectory = "bulk items",
    [string]$PreviewFile = "preview.txt",
    [string]$NestedPreviewFile = "bulk items\\Nested Folder\\deep-note.txt",
    [string]$CopySourceFile = "copy-source.bin"
  )

  $copyDestination = Join-Path ([System.IO.Path]::GetTempPath()) ("orchard-copy-" + [guid]::NewGuid() + ".bin")
  try {
    $rootFirst = Get-ChildItem -LiteralPath "$MountPoint\\" | Select-Object -ExpandProperty Name
    $rootSecond = Get-ChildItem -LiteralPath "$MountPoint\\" | Select-Object -ExpandProperty Name
    $bulkFirst =
      Get-ChildItem -LiteralPath (Join-Path $MountPoint $LargeDirectory) | Select-Object -ExpandProperty Name
    $bulkSecond =
      Get-ChildItem -LiteralPath (Join-Path $MountPoint $LargeDirectory) | Select-Object -ExpandProperty Name
    $previewText = [System.IO.File]::ReadAllText((Join-Path $MountPoint $PreviewFile))
    $nestedPreviewText = [System.IO.File]::ReadAllText((Join-Path $MountPoint $NestedPreviewFile))

    Copy-Item -LiteralPath (Join-Path $MountPoint $CopySourceFile) -Destination $copyDestination
    $mountedHash = (Get-FileHash -LiteralPath (Join-Path $MountPoint $CopySourceFile)).Hash
    $copiedHash = (Get-FileHash -LiteralPath $copyDestination).Hash

    if ($rootFirst.Count -ne $rootSecond.Count -or
        (@($rootFirst) -join "`n") -ne (@($rootSecond) -join "`n")) {
      throw "Repeated root enumeration did not return a stable listing."
    }
    if ($bulkFirst.Count -ne $bulkSecond.Count -or
        (@($bulkFirst) -join "`n") -ne (@($bulkSecond) -join "`n")) {
      throw "Repeated large-directory enumeration did not return a stable listing."
    }
    if ($bulkFirst.Count -lt 100) {
      throw "Large-directory smoke fixture returned too few entries: $($bulkFirst.Count)."
    }
    if ($mountedHash -ne $copiedHash) {
      throw "Copy-out hash mismatch between the mounted file and copied file."
    }

    return [pscustomobject]@{
      root_entries = @($rootFirst)
      large_directory_entry_count = $bulkFirst.Count
      first_large_directory_entry = if ($bulkFirst.Count -gt 0) { $bulkFirst[0] } else { $null }
      last_large_directory_entry = if ($bulkFirst.Count -gt 0) { $bulkFirst[-1] } else { $null }
      preview_text = $previewText.TrimEnd("`r", "`n")
      nested_preview_text = $nestedPreviewText.TrimEnd("`r", "`n")
      copy_source_hash = $mountedHash
    }
  }
  finally {
    if (Test-Path $copyDestination) {
      Remove-Item -LiteralPath $copyDestination -Force
    }
  }
}

function Invoke-OrchardLinkBehaviorChecks {
  param(
    [Parameter(Mandatory = $true)]
    [string]$MountPoint,

    [string]$RelativeLinkFile = "a-relative-note-link.txt",
    [string]$AbsoluteLinkFile = "absolute-alpha-link.txt",
    [string]$BrokenLinkFile = "broken-link.txt",
    [string]$HardLinkA = "hard-a.txt",
    [string]$HardLinkB = "hard-b.txt",
    [string]$CrossDirectoryAlias = "note-link.txt"
  )

  $relativeItem = Get-Item -LiteralPath (Join-Path $MountPoint $RelativeLinkFile)
  $absoluteItem = Get-Item -LiteralPath (Join-Path $MountPoint $AbsoluteLinkFile)
  $relativeText = [System.IO.File]::ReadAllText((Join-Path $MountPoint $RelativeLinkFile))
  $absoluteText = [System.IO.File]::ReadAllText((Join-Path $MountPoint $AbsoluteLinkFile))
  $aliasText = [System.IO.File]::ReadAllText((Join-Path $MountPoint $CrossDirectoryAlias))
  $hardAHash = (Get-FileHash -LiteralPath (Join-Path $MountPoint $HardLinkA)).Hash
  $hardBHash = (Get-FileHash -LiteralPath (Join-Path $MountPoint $HardLinkB)).Hash
  $brokenLinkText = [System.IO.File]::ReadAllText((Join-Path $MountPoint $BrokenLinkFile))

  if ($relativeText -ne "Nested note`n") {
    throw "Relative symlink read returned unexpected content."
  }
  if ($absoluteText -ne "Hello Orchard`n") {
    throw "Absolute symlink read returned unexpected content."
  }
  if ($aliasText -ne "Nested note`n") {
    throw "Cross-directory hard-link alias returned unexpected content."
  }
  if ($hardAHash -ne $hardBHash) {
    throw "Hard-link aliases did not return matching hashes."
  }
  if ($brokenLinkText -ne "/missing/ghost.txt") {
    throw "Broken symlink fallback returned unexpected content."
  }

  return [pscustomobject]@{
    relative_link_attributes = $relativeItem.Attributes.ToString()
    absolute_link_attributes = $absoluteItem.Attributes.ToString()
    relative_link_text = $relativeText.TrimEnd("`r", "`n")
    absolute_link_text = $absoluteText.TrimEnd("`r", "`n")
    hard_link_hash = $hardAHash
    cross_directory_alias_text = $aliasText.TrimEnd("`r", "`n")
    broken_link_text = $brokenLinkText
  }
}
