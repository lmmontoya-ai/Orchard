param(
  [Parameter(Mandatory = $true)]
  [string]$ToolName,

  [string[]]$CandidatePaths = @()
)

$command = Get-Command $ToolName -ErrorAction SilentlyContinue
if ($command) {
  return $command.Source
}

foreach ($candidate in $CandidatePaths) {
  if ([string]::IsNullOrWhiteSpace($candidate)) {
    continue
  }

  if (Test-Path $candidate) {
    return $candidate
  }
}

throw "Unable to locate required tool '$ToolName'. Checked PATH and candidate paths."

