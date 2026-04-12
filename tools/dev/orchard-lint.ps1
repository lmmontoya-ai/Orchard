param(
  [string]$BuildDir = "build/default"
)

$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $true

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$resolveToolScript = Join-Path $PSScriptRoot "Resolve-OrchardTool.ps1"

if ([System.IO.Path]::IsPathRooted($BuildDir)) {
  $resolvedBuildDir = $BuildDir
} else {
  $resolvedBuildDir = Join-Path $repoRoot $BuildDir
}

$compileCommands = Join-Path $resolvedBuildDir "compile_commands.json"

if (-not (Test-Path $compileCommands)) {
  throw "Missing compile_commands.json at '$compileCommands'. Run 'cmake --preset default' first."
}

$clangTidy = & $resolveToolScript `
  -ToolName "clang-tidy" `
  -CandidatePaths @(
    "C:\Users\luism\miniconda3\Library\bin\clang-tidy.exe",
    "C:\Program Files\LLVM\bin\clang-tidy.exe"
  )

$git = & $resolveToolScript -ToolName "git"
$files = & $git -C $repoRoot ls-files -- '*.cc' '*.cpp' '*.cxx'

if (-not $files) {
  Write-Host "No translation units matched the lint scope."
  exit 0
}

foreach ($relativePath in $files) {
  $fullPath = Join-Path $repoRoot $relativePath
  & $clangTidy "--quiet" "-p=$resolvedBuildDir" $fullPath
  if ($LASTEXITCODE -ne 0) {
    throw "clang-tidy failed for '$relativePath'."
  }
}

Write-Host "clang-tidy passed for $($files.Count) translation unit(s)."
