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

. (Join-Path $PSScriptRoot "OrchardWinFspTestCommon.ps1")

$repoRoot = Get-OrchardRepoRoot -ScriptRoot $PSScriptRoot
if (-not $TargetPath) {
  $TargetPath = Join-Path $repoRoot "tests\\corpus\\samples\\explorer-large.img"
}

$session = Start-OrchardMountSmokeSession `
  -RepoRoot $repoRoot `
  -TargetPath $TargetPath `
  -MountPoint $MountPoint `
  -HoldMs $HoldMs

try {
  $checkResult = Invoke-OrchardExplorerStressChecks `
    -MountPoint $session.mount_point `
    -LargeDirectory $LargeDirectory `
    -PreviewFile $PreviewFile `
    -NestedPreviewFile $NestedPreviewFile `
    -CopySourceFile $CopySourceFile

  [pscustomobject]@{
    mount_point = $session.mount_point
    mount_elapsed_ms = $session.mount_elapsed_ms
    root_entries = $checkResult.root_entries
    large_directory_entry_count = $checkResult.large_directory_entry_count
    first_large_directory_entry = $checkResult.first_large_directory_entry
    last_large_directory_entry = $checkResult.last_large_directory_entry
    preview_text = $checkResult.preview_text
    nested_preview_text = $checkResult.nested_preview_text
    copy_source_hash = $checkResult.copy_source_hash
  } | ConvertTo-Json -Depth 4
}
finally {
  [void](Stop-OrchardMountSmokeSession -Session $session)
}
