param([string]$QtRoot = "$PSScriptRoot\..\.tools\Qt\6.8.3\msvc2022_64")
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..").Path
$cmake = "$root\.tools\python\cmake\data\bin\cmake.exe"
$ninja = "$root\.tools\python\bin\ninja.exe"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Microsoft C++ Build Tools is missing.' }
if (!(Test-Path "$QtRoot\bin\qmake.exe")) { throw 'Qt 6.8.3 msvc2022_64 is missing.' }
# Import the native x64 compiler environment, without changing machine settings.
$devcmd = Join-Path $vs 'Common7\Tools\VsDevCmd.bat'
$environment = & $env:ComSpec /d /s /c "`"`"$devcmd`" -no_logo -arch=x64 -host_arch=x64 -vcvars_ver=14.44.35207 -winsdk=10.0.26100.0 >nul && set`""
foreach ($line in $environment) {
  if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
}
$env:VSLANG = '1033'
& $cmake -S $root -B "$root\build" -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_PREFIX_PATH=$QtRoot" -DCMAKE_BUILD_TYPE=Release '-DCMAKE_CL_SHOWINCLUDES_PREFIX=Note: including file: '
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
& $cmake --build "$root\build" --parallel
if ($LASTEXITCODE -ne 0) { throw 'Compilation failed.' }
& "$PSScriptRoot\deploy-runtime.ps1" -Destination "$root\build" -QtRoot $QtRoot
