# Build both KiTTY MSI installers with WiX v5 (the `wix` dotnet tool).
#   prereq:  dotnet tool install --global wix --version 5.0.2
#   sources: the packaged binaries in release-084\ (build them first)
# Runs on WINDOWS (wix is a Windows dotnet tool); the .wxs use bare filenames
# resolved via -bindpath. Output MSIs land in the release dir.
param(
  [string]$Ver = "0.84.0.5-beta",
  [string]$Rel = "C:\build\release-084"
)
$ErrorActionPreference = "Stop"
$env:Path += ";$env:USERPROFILE\.dotnet\tools"
$here = Split-Path -Parent $PSCommandPath
foreach ($scope in "peruser","system") {
  $out = Join-Path $Rel "KiTTY-$Ver-x64-$scope.msi"
  Remove-Item $out -ErrorAction SilentlyContinue
  # second bindpath = the windows/ source dir, so the launcher shortcut's
  # <Icon SourceFile="kitty_icons\icon_25.ico"> resolves (it's not in $Rel).
  wix build -arch x64 -ext WixToolset.Util.wixext -bindpath $Rel -bindpath (Split-Path $here -Parent) -o $out (Join-Path $here "kitty-$scope.wxs")
  if (-not (Test-Path $out)) { throw "build failed: $scope" }
  Write-Host ("built {0} ({1:N2} MB)" -f $out, ((Get-Item $out).Length/1MB))
}
