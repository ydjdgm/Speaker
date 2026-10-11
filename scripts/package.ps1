param([string]$QtRoot = "$PSScriptRoot\..\.tools\Qt\6.8.3\msvc2022_64")
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..").Path
$destination = "$root\dist\Speaker"
New-Item -ItemType Directory -Force $destination | Out-Null
Copy-Item -LiteralPath "$root\build\Speaker.exe" -Destination $destination -Force
& "$PSScriptRoot\deploy-runtime.ps1" -Destination $destination -QtRoot $QtRoot
foreach ($folder in @('licenses','docs','translations','sources','scripts')) { New-Item -ItemType Directory -Force "$destination\$folder" | Out-Null }
Copy-Item "$root\licenses\*" "$destination\licenses" -Recurse -Force
Copy-Item "$root\docs\*" "$destination\docs" -Recurse -Force
Copy-Item "$root\.tools\qtbase-source.zip" "$destination\sources\qtbase-everywhere-src-6.8.3.zip" -Force
Copy-Item "$root\.tools\qttranslations-source.zip" "$destination\sources\qttranslations-everywhere-src-6.8.3.zip" -Force
Copy-Item "$root\scripts\create-test-volume.ps1" "$destination\scripts" -Force
Compress-Archive -Path "$destination\*" -DestinationPath "$root\dist\Speaker-1.0.0-windows-x64.zip" -Force
Write-Output "$destination\Speaker.exe"
