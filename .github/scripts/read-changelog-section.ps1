param(
  [Parameter(Mandatory = $true)]
  [string]$Version,
  [string]$ChangelogFile = (Join-Path $PSScriptRoot "..\..\CHANGELOG.md")
)

$ErrorActionPreference = "Stop"
$resolved = Resolve-Path -LiteralPath $ChangelogFile
$contents = Get-Content -Raw -LiteralPath $resolved
$escapedVersion = [regex]::Escape($Version)
$pattern = "(?ms)^##[ \t]+$escapedVersion(?:[ \t]+[^\r\n]*)?\r?\n.*?(?=^##[ \t]+|\z)"
$matches = [regex]::Matches($contents, $pattern)
if ($matches.Count -ne 1) {
  throw "Expected exactly one CHANGELOG.md section for version $Version in $resolved."
}

$matches[0].Value.TrimEnd()
