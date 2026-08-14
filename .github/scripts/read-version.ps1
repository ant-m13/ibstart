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
$semver = '^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-((?:0|[1-9]\d*|[0-9]*[A-Za-z-][0-9A-Za-z-]*)(?:\.(?:0|[1-9]\d*|[0-9]*[A-Za-z-][0-9A-Za-z-]*))*))?$'
if ($version -notmatch $semver) {
  throw "IBSTART_VERSION '$version' is not valid SemVer without build metadata."
}

$version
