param(
  [string]$TargetPath,
  [string]$MountPoint,
  [int]$HoldMs = 15000,
  [string]$LargeDirectory = "bulk items",
  [string]$PreviewFile = "preview.txt",
  [string]$NestedPreviewFile = "bulk items\\Nested Folder\\deep-note.txt",
  [string]$CopySourceFile = "copy-source.bin"
)

$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $true

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $TargetPath) {
  $TargetPath = Join-Path $repoRoot "tests\\corpus\\samples\\explorer-large.img"
}

$resolvedTargetPath = (Resolve-Path $TargetPath).Path
$mountSmokeExecutable = Join-Path $repoRoot "build\\default\\tools\\mount-smoke\\orchard-mount-smoke.exe"
if (-not (Test-Path $mountSmokeExecutable)) {
  throw "Missing orchard-mount-smoke executable at '$mountSmokeExecutable'. Build the repo first."
}

if (-not $MountPoint) {
  $freeDriveLetter =
    @("R", "S", "T", "U", "V", "W", "X", "Y", "Z") |
    Where-Object { -not (Get-PSDrive -Name $_ -ErrorAction SilentlyContinue) } |
    Select-Object -First 1
  if (-not $freeDriveLetter) {
    throw "No free drive letter is available for the WinFsp shell smoke run."
  }
  $MountPoint = "$freeDriveLetter`:"
}

$copyDestination = Join-Path ([System.IO.Path]::GetTempPath()) ("orchard-copy-" + [guid]::NewGuid() + ".bin")
$mountProcess =
  Start-Process -FilePath $mountSmokeExecutable `
    -ArgumentList @("--target", $resolvedTargetPath, "--mountpoint", $MountPoint, "--hold-ms", $HoldMs) `
    -PassThru `
    -WindowStyle Hidden

try {
  $mounted = $false
  for ($attempt = 0; $attempt -lt 60; ++$attempt) {
    Start-Sleep -Milliseconds 250
    if (Test-Path "$MountPoint\\") {
      $mounted = $true
      break
    }
    if ($mountProcess.HasExited) {
      throw "orchard-mount-smoke exited early with code $($mountProcess.ExitCode)."
    }
  }

  if (-not $mounted) {
    throw "The mount point '$MountPoint' did not appear before timeout."
  }

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

  [pscustomobject]@{
    mount_point = $MountPoint
    root_entries = @($rootFirst)
    large_directory_entry_count = $bulkFirst.Count
    first_large_directory_entry = if ($bulkFirst.Count -gt 0) { $bulkFirst[0] } else { $null }
    last_large_directory_entry = if ($bulkFirst.Count -gt 0) { $bulkFirst[-1] } else { $null }
    preview_text = $previewText.TrimEnd("`r", "`n")
    nested_preview_text = $nestedPreviewText.TrimEnd("`r", "`n")
    copy_source_hash = $mountedHash
  } | ConvertTo-Json -Depth 4
}
finally {
  if (Test-Path $copyDestination) {
    Remove-Item -LiteralPath $copyDestination -Force
  }
  if (-not $mountProcess.HasExited) {
    $mountProcess.WaitForExit()
  }
  Start-Sleep -Milliseconds 500
  if (Test-Path "$MountPoint\\") {
    throw "Mount point '$MountPoint' still exists after orchard-mount-smoke exited."
  }
}
