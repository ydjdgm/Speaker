param([string]$QtRoot = "$PSScriptRoot\..\.tools\Qt\6.8.3\msvc2022_64")
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..").Path
$destination = "$root\dist\Speaker"
New-Item -ItemType Directory -Force $destination | Out-Null
Copy-Item -LiteralPath "$root\build\Speaker.exe" -Destination $destination -Force
& "$QtRoot\bin\windeployqt.exe" --release --no-translations --no-compiler-runtime --no-system-d3d-compiler --no-system-dxc-compiler --no-opengl-sw --skip-plugin-types generic,networkinformation,tls --dir $destination "$destination\Speaker.exe"
if ($LASTEXITCODE -ne 0) { throw 'Qt deployment failed.' }
foreach ($folder in @('licenses','docs','translations','sources','scripts')) { New-Item -ItemType Directory -Force "$destination\$folder" | Out-Null }
Copy-Item "$root\licenses\*" "$destination\licenses" -Recurse -Force
Copy-Item "$root\docs\*" "$destination\docs" -Recurse -Force
Copy-Item "$QtRoot\translations\qtbase_ko.qm" "$destination\translations" -Force
@('[Paths]','Prefix=.','Plugins=.','Translations=translations') | Set-Content "$destination\qt.conf" -Encoding ascii
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$crt=Join-Path $vs 'VC\Redist\MSVC\14.44.35112\x64\Microsoft.VC143.CRT'
if (!(Test-Path "$crt\msvcp140.dll")) { throw 'Pinned MSVC redistributable files are missing.' }
Copy-Item "$crt\*.dll" $destination -Force
Copy-Item "$root\.tools\qtbase-source.zip" "$destination\sources\qtbase-everywhere-src-6.8.3.zip" -Force
Copy-Item "$root\.tools\qttranslations-source.zip" "$destination\sources\qttranslations-everywhere-src-6.8.3.zip" -Force
Copy-Item "$root\scripts\create-test-volume.ps1" "$destination\scripts" -Force
Compress-Archive -Path "$destination\*" -DestinationPath "$root\dist\Speaker-1.0.0-windows-x64.zip" -Force
Write-Output "$destination\Speaker.exe"
