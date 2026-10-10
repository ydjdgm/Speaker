#requires -RunAsAdministrator
[CmdletBinding(DefaultParameterSetName='Create')]
param([Parameter(ParameterSetName='Create')][switch]$Create,
      [Parameter(Mandatory,ParameterSetName='Detach')][switch]$Detach)
$ErrorActionPreference='Stop'
# User final-test helper only. It never selects, formats or deletes a physical disk.
$base=Join-Path $env:LOCALAPPDATA 'SpeakerFinalTestVolume'
$image=Join-Path $base 'speaker-test.vhd'
$mount=Join-Path $base 'Mount'
New-Item -ItemType Directory -Force $base | Out-Null
if ($Detach) {
    if (!(Test-Path -LiteralPath $image)) { throw 'The test VHD does not exist.' }
    $commands=@("select vdisk file=`"$image`"",'detach vdisk','exit')
} else {
    if (Test-Path -LiteralPath $image) { throw "A test image already exists at $image. Use -Detach after testing; the original is retained. To repeat, rename the detached image in Explorer first." }
    New-Item -ItemType Directory -Force $mount | Out-Null
    if ((Get-ChildItem -LiteralPath $mount -Force | Measure-Object).Count) { throw 'The mount directory must be empty.' }
    $commands=@("create vdisk file=`"$image`" maximum=64 type=expandable", "select vdisk file=`"$image`"",'attach vdisk','create partition primary','format fs=ntfs quick label=SPEAKER_TEST',"assign mount=`"$mount`"",'exit')
}
$commandFile=Join-Path $base 'diskpart.txt'
[IO.File]::WriteAllLines($commandFile,$commands,[Text.Encoding]::Unicode)
& "$env:SystemRoot\System32\diskpart.exe" /s $commandFile
if ($LASTEXITCODE -ne 0) { throw 'DiskPart failed. Keep its output for diagnosis; no automatic cleanup is performed.' }
if (!$Detach) {
    $volume=Get-Volume -FilePath ($mount+'\')
    if ($volume.FileSystemLabel -ne 'SPEAKER_TEST' -or $volume.Size -gt 70MB) { throw 'The expected 64 MB test volume was not mounted. Do not use this folder for testing.' }
    $folder=Join-Path $mount 'Recordings'
    New-Item -ItemType Directory -Path $folder -Force | Out-Null
    Write-Output "Select this recording folder in Speaker: $folder"
    Write-Output 'Record for up to 7 minutes to fill only this 64 MB virtual volume.'
} else {
    Write-Output "Detached. The VHD and any test recordings are preserved at $image."
}
