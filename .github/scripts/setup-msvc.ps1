$ErrorActionPreference = "Stop"

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) {
  throw "vswhere.exe was not found on the Windows runner."
}

$installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $installation) {
  throw "A Visual Studio installation with the MSVC x64 tools was not found."
}

$developerCommand = Join-Path ($installation | Select-Object -First 1) "Common7\Tools\VsDevCmd.bat"
if (-not (Test-Path -LiteralPath $developerCommand)) {
  throw "VsDevCmd.bat was not found in the selected Visual Studio installation."
}

$batch = Join-Path $env:RUNNER_TEMP "ibstart-msvc-environment.cmd"
@"
@call "$developerCommand" -no_logo -arch=x64 -host_arch=x64
@if errorlevel 1 exit /b %errorlevel%
@set
"@ | Set-Content -LiteralPath $batch -Encoding ascii

$lines = & $env:ComSpec /d /s /c "`"$batch`""
if ($LASTEXITCODE -ne 0) {
  throw "VsDevCmd.bat failed with exit code $LASTEXITCODE."
}

$required = @("PATH", "INCLUDE", "LIB", "LIBPATH")
$environment = @{}
foreach ($line in $lines) {
  $separator = $line.IndexOf("=")
  if ($separator -le 0) { continue }
  $name = $line.Substring(0, $separator)
  if ($required -contains $name) {
    $environment[$name] = $line.Substring($separator + 1)
  }
}

foreach ($name in $required) {
  if (-not $environment.ContainsKey($name)) {
    throw "VsDevCmd.bat did not define the required $name environment variable."
  }
  "$name=$($environment[$name])" | Add-Content -LiteralPath $env:GITHUB_ENV -Encoding utf8
}

