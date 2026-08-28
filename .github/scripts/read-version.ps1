param(
  [string]$VersionFile = (Join-Path $PSScriptRoot "..\..\cmake\IBStartVersion.cmake")
)

$ErrorActionPreference = "Stop"
$resolved = Resolve-Path -LiteralPath $VersionFile
$contents = Get-Content -Raw -LiteralPath $resolved
$matches = [regex]::Matches($contents, '(?m)^\s*set\(IBSTART_VERSION\s+"([^"]+)"\)\s*$')
if ($matches.Count -ne 1) {
  throw "Expected exactly one IBSTART_VERSION declaration in $resolved."
}

$version = $matches[0].Groups[1].Value
$semverPath = Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..\cmake\IBStartSemVer.regex")
$semver = (Get-Content -Raw -LiteralPath $semverPath).Trim()
if ([string]::IsNullOrWhiteSpace($semver)) {
  throw "The shared SemVer regular expression is empty: $semverPath."
}
if ($version -notmatch $semver) {
  throw "IBSTART_VERSION '$version' is not valid SemVer without build metadata."
}

$version
