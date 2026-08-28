$ErrorActionPreference = "Stop"

$regexPath = Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..\cmake\IBStartSemVer.regex")
$semver = (Get-Content -Raw -LiteralPath $regexPath).Trim()
if ([string]::IsNullOrWhiteSpace($semver)) {
  throw "The shared SemVer regular expression is empty: $regexPath."
}
$regex = [regex]::new($semver)
$readVersion = Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "read-version.ps1")

$validVersions = @(
  "0.0.0",
  "1.2.3",
  "1.2.3-0",
  "1.2.3-alpha",
  "1.2.3-alpha.1",
  "1.2.3-0alpha",
  "1.2.3-alpha-1",
  "1.2.3-alpha.0"
)
$invalidVersions = @(
  "00.1.2",
  "1.02.3",
  "1.2.03",
  "1.2.3-",
  "1.2.3-alpha.",
  "1.2.3-.alpha",
  "1.2.3-alpha..1",
  "1.2.3-alpha...1",
  "1.2.3-01",
  "1.2.3-alpha.01",
  "1.2.3+build",
  "1.2.3-alpha_1"
)

$temporaryFiles = [System.Collections.Generic.List[string]]::new()
try {
  foreach ($version in $validVersions) {
    if (-not $regex.IsMatch($version)) {
      throw "PowerShell rejected valid SemVer '$version'."
    }
    $file = New-TemporaryFile
    $temporaryFiles.Add($file.FullName)
    Set-Content -LiteralPath $file.FullName -Value "set(IBSTART_VERSION `"$version`")" -Encoding ascii -NoNewline
    $actual = (& $readVersion -VersionFile $file.FullName | Out-String).Trim()
    if ($actual -ne $version) {
      throw "read-version.ps1 returned '$actual' for valid SemVer '$version'."
    }
  }

  foreach ($version in $invalidVersions) {
    if ($regex.IsMatch($version)) {
      throw "PowerShell accepted invalid SemVer '$version'."
    }
    $file = New-TemporaryFile
    $temporaryFiles.Add($file.FullName)
    Set-Content -LiteralPath $file.FullName -Value "set(IBSTART_VERSION `"$version`")" -Encoding ascii -NoNewline
    $accepted = $false
    try {
      $null = & $readVersion -VersionFile $file.FullName
      $accepted = $true
    } catch {
    }
    if ($accepted) {
      throw "read-version.ps1 accepted invalid SemVer '$version'."
    }
  }
} finally {
  foreach ($file in $temporaryFiles) {
    Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue
  }
}

Write-Output "PowerShell SemVer validation tests passed."
