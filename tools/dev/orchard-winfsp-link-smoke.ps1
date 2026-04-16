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

. (Join-Path $PSScriptRoot "OrchardWinFspTestCommon.ps1")

$repoRoot = Get-OrchardRepoRoot -ScriptRoot $PSScriptRoot
if (-not $TargetPath) {
  $TargetPath = Join-Path $repoRoot "tests\\corpus\\samples\\link-behavior.img"
}

$session = Start-OrchardMountSmokeSession `
  -RepoRoot $repoRoot `
  -TargetPath $TargetPath `
  -MountPoint $MountPoint `
  -HoldMs $HoldMs

try {
  $checkResult = Invoke-OrchardLinkBehaviorChecks `
    -MountPoint $session.mount_point `
    -RelativeLinkFile $RelativeLinkFile `
    -AbsoluteLinkFile $AbsoluteLinkFile `
    -BrokenLinkFile $BrokenLinkFile `
    -HardLinkA $HardLinkA `
    -HardLinkB $HardLinkB `
    -CrossDirectoryAlias $CrossDirectoryAlias

  [pscustomobject]@{
    mount_point = $session.mount_point
    mount_elapsed_ms = $session.mount_elapsed_ms
    relative_link_attributes = $checkResult.relative_link_attributes
    absolute_link_attributes = $checkResult.absolute_link_attributes
    relative_link_text = $checkResult.relative_link_text
    absolute_link_text = $checkResult.absolute_link_text
    hard_link_hash = $checkResult.hard_link_hash
    cross_directory_alias_text = $checkResult.cross_directory_alias_text
    broken_link_text = $checkResult.broken_link_text
  } | ConvertTo-Json -Depth 4
}
finally {
  [void](Stop-OrchardMountSmokeSession -Session $session)
}
