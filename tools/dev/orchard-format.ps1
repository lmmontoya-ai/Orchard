param(
  [switch]$Check
)

$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $true

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$resolveToolScript = Join-Path $PSScriptRoot "Resolve-OrchardTool.ps1"

$clangFormat = & $resolveToolScript `
  -ToolName "clang-format" `
  -CandidatePaths @(
    "C:\Users\luism\miniconda3\Library\bin\clang-format.exe",
    "C:\Program Files\LLVM\bin\clang-format.exe"
  )

$git = & $resolveToolScript -ToolName "git"

$files = & $git -C $repoRoot ls-files -- '*.h' '*.hpp' '*.hh' '*.hxx' '*.c' '*.cc' '*.cpp' '*.cxx' '*.ixx'
if (-not $files) {
  Write-Host "No C/C++ files matched the formatter scope."
  exit 0
}

$commonArgs = @("--style=file")
if ($Check) {
  $commonArgs += @("--dry-run", "--Werror")
} else {
  $commonArgs += "-i"
}

foreach ($relativePath in $files) {
  $fullPath = Join-Path $repoRoot $relativePath
  & $clangFormat @commonArgs $fullPath
  if ($LASTEXITCODE -ne 0) {
    throw "clang-format failed for '$relativePath'."
  }
}

if ($Check) {
  Write-Host "clang-format check passed for $($files.Count) file(s)."
} else {
  Write-Host "clang-format updated $($files.Count) file(s)."
}
