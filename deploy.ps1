<#
  deploy.ps1 - copy a built firmware to an SD card.

  Usage:  .\deploy.ps1 -Bin out\build-<N>\FWDV280.BIN -Drive E           # flash it at the next power-on
          .\deploy.ps1 -Bin out\build-<N>\FW96658T.BIN -Drive E -Card    # run it from the card, no flashing

  Flash (default): writes FWDV280.BIN plus an empty NVTDELFW folder to the card root. NVTDELFW tells the
  firmware to delete the file after flashing -- without it, the device reflashes on every boot.
  Then: safe-eject the card, insert it into the POWERED-OFF Reels, power on. Power LED on with a black
  screen for ~30 s = flashing; do NOT cut power. If the old build still boots, check the card: if
  FWDV280.BIN is still there, the flash never ran (see docs\BUILDING.md, "It didn't flash").

  Card (-Card): writes FW96658T.BIN to the card root. The bootloader runs it from RAM at every power-on while
  the card is in; the flash is never written. Remove the card (or the file) to boot the installed firmware.
  A FWDV280.BIN on the same card takes precedence: that boot flashes it first, the card file runs after.
#>
param(
  [Parameter(Mandatory=$true)][string]$Bin,
  [Parameter(Mandatory=$true)][string]$Drive,
  [switch]$Card
)
$ErrorActionPreference = "Stop"
$Drive = $Drive.TrimEnd(':','\')
if(-not (Test-Path $Bin)){ Write-Host "ERROR: $Bin not found" -ForegroundColor Red; exit 1 }
if(-not (Test-Path "${Drive}:\")){ Write-Host "ERROR: drive ${Drive}: not present" -ForegroundColor Red; exit 1 }
$vol = Get-Volume -DriveLetter $Drive
if($vol.FileSystem -ne "FAT32"){
  Write-Host "ERROR: ${Drive}: is $($vol.FileSystem); the bootloader only reads FAT32 (<= 32 GB)." -ForegroundColor Red; exit 1
}
if(Test-Path "${Drive}:\LDDV280.BIN"){
  Write-Host "ERROR: ${Drive}:\LDDV280.BIN would REPLACE THE BOOTLOADER. Remove it from the card first." -ForegroundColor Red; exit 1
}
if($Card){
  Copy-Item $Bin "${Drive}:\FW96658T.BIN" -Force
  Write-VolumeCache -DriveLetter $Drive
  Write-Host "deployed -> ${Drive}:\FW96658T.BIN ($((Get-Item "${Drive}:\FW96658T.BIN").Length) bytes, cache flushed)"
  if(Test-Path "${Drive}:\FWDV280.BIN"){
    Write-Host "NOTE: ${Drive}:\FWDV280.BIN is also on the card: the next boot FLASHES it first." -ForegroundColor Yellow
  }
  Write-Host "Now safe-eject, insert into the powered-OFF Reels, and power on (runs from the card, no flashing)."
  exit 0
}
Copy-Item $Bin "${Drive}:\FWDV280.BIN" -Force
New-Item -ItemType Directory -Force "${Drive}:\NVTDELFW" | Out-Null
Write-VolumeCache -DriveLetter $Drive
Write-Host "deployed -> ${Drive}:\FWDV280.BIN ($((Get-Item "${Drive}:\FWDV280.BIN").Length) bytes) + NVTDELFW (cache flushed)"
if(Test-Path "${Drive}:\FW96658T.BIN"){
  Write-Host "NOTE: ${Drive}:\FW96658T.BIN (card file) is also on the card: after flashing, it runs on top." -ForegroundColor Yellow
}
Write-Host "Now safe-eject, insert into the powered-OFF Reels, and power on."
