<#
  deploy.ps1 - copy a built FWDV280.BIN to an SD card so the Reels flashes it at the next power-on.

  Usage:  .\deploy.ps1 -Bin out\build-<N>\FWDV280.BIN -Drive E

  Writes FWDV280.BIN plus an empty NVTDELFW folder to the card root. NVTDELFW tells the bootloader to
  delete the file after flashing -- without it, the device reflashes on every boot.

  Then: safe-eject the card, insert it into the POWERED-OFF Reels, power on. Power LED on with a black
  screen for ~30 s = flashing; do NOT cut power. If the old build still boots, check the card: if
  FWDV280.BIN is still there, the flash never ran (see docs\BUILDING.md, "It didn't flash").
#>
param(
  [Parameter(Mandatory=$true)][string]$Bin,
  [Parameter(Mandatory=$true)][string]$Drive
)
$ErrorActionPreference = "Stop"
$Drive = $Drive.TrimEnd(':','\')
if(-not (Test-Path $Bin)){ Write-Host "ERROR: $Bin not found" -ForegroundColor Red; exit 1 }
if(-not (Test-Path "${Drive}:\")){ Write-Host "ERROR: drive ${Drive}: not present" -ForegroundColor Red; exit 1 }
$vol = Get-Volume -DriveLetter $Drive
if($vol.FileSystem -ne "FAT32"){
  Write-Host "ERROR: ${Drive}: is $($vol.FileSystem); the bootloader only reads FAT32 (<= 32 GB)." -ForegroundColor Red; exit 1
}
Copy-Item $Bin "${Drive}:\FWDV280.BIN" -Force
New-Item -ItemType Directory -Force "${Drive}:\NVTDELFW" | Out-Null
Write-VolumeCache -DriveLetter $Drive
Write-Host "deployed -> ${Drive}:\FWDV280.BIN ($((Get-Item "${Drive}:\FWDV280.BIN").Length) bytes) + NVTDELFW (cache flushed)"
Write-Host "Now safe-eject, insert into the powered-OFF Reels, and power on."
