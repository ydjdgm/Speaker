param(
    [Parameter(Mandatory)][string]$Destination,
    [string]$QtRoot = "$PSScriptRoot\..\.tools\Qt\6.8.3\msvc2022_64"
)
$ErrorActionPreference = 'Stop'
$destinationPath = (Resolve-Path -LiteralPath $Destination).Path
$executable = Join-Path $destinationPath 'Speaker.exe'
if (!(Test-Path -LiteralPath $executable)) { throw 'Speaker.exe is missing from the deployment destination.' }
# Inspect the binary and copy dependencies; never launch the application.
& "$QtRoot\bin\windeployqt.exe" --release --no-translations --no-compiler-runtime --no-system-d3d-compiler --no-system-dxc-compiler --no-opengl-sw --skip-plugin-types generic,networkinformation,tls --dir $destinationPath $executable
if ($LASTEXITCODE -ne 0) { throw 'Qt runtime deployment failed.' }
New-Item -ItemType Directory -Force "$destinationPath\translations" | Out-Null
Copy-Item -LiteralPath "$QtRoot\translations\qtbase_ko.qm" -Destination "$destinationPath\translations" -Force
@('[Paths]','Prefix=.','Plugins=.','Translations=translations') | Set-Content "$destinationPath\qt.conf" -Encoding ascii
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Microsoft C++ Build Tools is missing.' }
$crt = Join-Path $vs 'VC\Redist\MSVC\14.44.35112\x64\Microsoft.VC143.CRT'
if (!(Test-Path "$crt\msvcp140.dll")) { throw 'Pinned MSVC redistributable files are missing.' }
Copy-Item "$crt\*.dll" $destinationPath -Force
foreach ($required in @('Qt6Core.dll','Qt6Gui.dll','Qt6Widgets.dll','platforms\qwindows.dll','translations\qtbase_ko.qm','msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll')) {
    if (!(Test-Path -LiteralPath (Join-Path $destinationPath $required))) { throw "Runtime deployment is incomplete: $required" }
}
