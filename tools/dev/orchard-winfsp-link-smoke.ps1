param(
  [string]$TargetPath,
  [string]$MountPoint,
  [int]$HoldMs = 15000,
  [string]$RelativeLinkFile = "a-relative-note-link.txt",
  [string]$AbsoluteLinkFile = "absolute-alpha-link.txt",
  [string]$BrokenLinkFile = "broken-link.txt",
  [string]$HardLinkA = "hard-a.txt",
  [string]$HardLinkB = "hard-b.txt",
  [string]$CrossDirectoryAlias = "note-link.txt"
)

$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $true

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $TargetPath) {
  $TargetPath = Join-Path $repoRoot "tests\\corpus\\samples\\link-behavior.img"
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
    throw "No free drive letter is available for the WinFsp link smoke run."
  }
  $MountPoint = "$freeDriveLetter`:"
}

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

  $relativeItem = Get-Item -LiteralPath (Join-Path $MountPoint $RelativeLinkFile)
  $absoluteItem = Get-Item -LiteralPath (Join-Path $MountPoint $AbsoluteLinkFile)
  $relativeText = [System.IO.File]::ReadAllText((Join-Path $MountPoint $RelativeLinkFile))
  $absoluteText = [System.IO.File]::ReadAllText((Join-Path $MountPoint $AbsoluteLinkFile))
  $aliasText = [System.IO.File]::ReadAllText((Join-Path $MountPoint $CrossDirectoryAlias))
  $hardAHash = (Get-FileHash -LiteralPath (Join-Path $MountPoint $HardLinkA)).Hash
  $hardBHash = (Get-FileHash -LiteralPath (Join-Path $MountPoint $HardLinkB)).Hash

  $brokenLinkFailed = $false
  try {
    [void][System.IO.File]::ReadAllText((Join-Path $MountPoint $BrokenLinkFile))
  }
  catch {
    $brokenLinkFailed = $true
  }

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
  if (-not $brokenLinkFailed) {
    throw "Broken symlink read unexpectedly succeeded."
  }
  if ($relativeItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint -eq 0) {
    throw "Relative symlink was not surfaced as a reparse point."
  }
  if ($absoluteItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint -eq 0) {
    throw "Absolute symlink was not surfaced as a reparse point."
  }

  [pscustomobject]@{
    mount_point = $MountPoint
    relative_link_attributes = $relativeItem.Attributes.ToString()
    absolute_link_attributes = $absoluteItem.Attributes.ToString()
    relative_link_text = $relativeText.TrimEnd("`r", "`n")
    absolute_link_text = $absoluteText.TrimEnd("`r", "`n")
    hard_link_hash = $hardAHash
    cross_directory_alias_text = $aliasText.TrimEnd("`r", "`n")
    broken_link_failed = $brokenLinkFailed
  } | ConvertTo-Json -Depth 4
}
finally {
  if (-not $mountProcess.HasExited) {
    $mountProcess.WaitForExit()
  }
  Start-Sleep -Milliseconds 500
  if (Test-Path "$MountPoint\\") {
    throw "Mount point '$MountPoint' still exists after orchard-mount-smoke exited."
  }
}
