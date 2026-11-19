<#
  build.ps1 - build the Kodak Reels Type D firmware mod.

  Pipeline:
    1. compile + link src/osdmenu.c        (WSL: mipsel-linux-gnu-gcc / ld)
    2. splice into the base + patch        (python tools/osd_build.py -> build/vd-osd.rbn)
    3. checksum + pack                     (bin/ntkcalcVS.exe, bin/bfc4ntkVS.exe -> build/vd-osd.bcl)
    4. stage                               (out/build-<N>/FWDV280.BIN + manifest.txt)
    5. optional: patch-loader              (-Card: python tools/make_pldr.py -> out/build-<N>/FW96658T.BIN)
    6. optional: copy to an SD card        (-Drive E; with -Card the card file, otherwise the flash file)

  Usage:
    .\build.ps1 -Build <N>                  # build, stage to out\build-<N>\
    .\build.ps1 -Build <N> -Drive E         # ...and copy it to the SD card in E:
    .\build.ps1 -Build <N> -Resolution native
    .\build.ps1 -Build <N> -NoHooks         # diagnostic: constant patches only, none of the mod runs
    .\build.ps1 -Build <N> -NoSplash        # original "Type D - WIP" boot splash
    .\build.ps1 -Build <N> -Date YYYY-MM-DD # splash date (default: today) -- rebuild a release byte-identically
                                            # (each CHANGELOG entry lists its exact command and SHA256)
    .\build.ps1 -Build <N> -Card            # also build FW96658T.BIN: runs from the SD card, never flashes
    .\build.ps1 -Build <N> -Card -Dump <f>  # ...and test it in an emulator against a flash dump (pldr\README.md)
    .\build.ps1 -Build <N> -Card -Drive E   # ...and copy FW96658T.BIN (not FWDV280.BIN) to the SD card in E:

  -Build is the number shown on screen as "FW <N>" -- bump it for every build you flash.

  Needs: WSL with the mipsel-linux-gnu cross toolchain, Python 3 on Windows, videodoctor's FWDV280.BIN
  in base\ (unpacked automatically on first run) and the two packing tools in bin\
  (see base\README.md, bin\README.md, docs\BUILDING.md).
#>
param(
  [Parameter(Mandatory=$true)][int]$Build,
  [ValidateSet("std","native")][string]$Resolution = "std",
  [ValidateSet("x1","x2","x4")][string]$Bitrate = "x4",   # only used with -NoHooks (otherwise it's a menu item)
  [string]$Drive = "",
  [switch]$NoHooks,
  [switch]$NoSplash,  # keep the original "Type D - WIP" boot splash
  [string]$Date = (Get-Date -Format "yyyy-MM-dd"),  # build date shown on the boot splash
  [switch]$Card,      # also build the patch-loader FW96658T.BIN (run from the SD card, see pldr\README.md)
  [string]$Dump = ""  # with -Card: emulator test against this SPI flash dump (needs: pip install unicorn)
)
$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot
function Step([string]$m){ Write-Host "`n=== $m ===" -ForegroundColor Cyan }
function Die([string]$m){ Write-Host "ERROR: $m" -ForegroundColor Red; exit 1 }
function CheckExit([string]$what){ if($LASTEXITCODE -ne 0){ Die "$what failed (exit $LASTEXITCODE)" } }
# Native tools: judge them by exit code, not stderr. Windows PowerShell 5.1 turns ANY stderr line from a
# native command into a terminating error once output is redirected -- even a harmless warning.
function Native([scriptblock]$sb){ $old = $ErrorActionPreference; $ErrorActionPreference = "Continue"
  try { & $sb } finally { $ErrorActionPreference = $old } }

foreach($f in "bin\ntkcalcVS.exe","bin\bfc4ntkVS.exe"){ if(-not (Test-Path "$Root\$f")){ Die "$f missing - see bin\README.md" } }
if(-not (Test-Path "$Root\base\vd-FWDV280-D.rbn")){
  # First run: unpack videodoctor's packed release (the FWDV280.BIN from the forum) into the raw base image.
  # osd_build.py verifies its SHA256, so a wrong version fails.
  if(-not (Test-Path "$Root\base\FWDV280.BIN")){ Die "no base firmware in base\ - see base\README.md" }
  Step "0/4 unpack base\FWDV280.BIN -> base\vd-FWDV280-D.rbn"
  Push-Location "$Root\base"
  try { Native { & "$Root\bin\bfc4ntkVS.exe" -d FWDV280.BIN vd-FWDV280-D.rbn | Out-Null }; CheckExit "bfc4ntk (decompress)" }
  finally { Pop-Location }
}
New-Item -ItemType Directory -Force "$Root\build" | Out-Null

# 1) compile + link -------------------------------------------------------------------------------
Step "1/4 compile + link (WSL)"
$wslRoot = (& wsl.exe wslpath -a ($Root -replace '\\','/')).Trim()
if(-not $wslRoot){ Die "WSL not available (wsl.exe wslpath failed)" }
$cflags = "-DFW_BUILD=$Build -march=mips32 -EL -ffreestanding -nostdlib -nodefaultlibs " +
          "-fomit-frame-pointer -fno-stack-protector -Os -mno-abicalls -fno-pic -fno-reorder-blocks"
# The mod is linked to run at 0x8034116c (free space in the base). orig = the stock WB function the
# per-frame hook tail-calls; resolve = the firmware's display-pool getter.
$lflags = "-EL -Ttext=0x8034116c -e 0x8034116c --defsym orig=0x802b7b7c --defsym resolve=0x80006500 --defsym set_exp=0x801df99c --defsym set_gain=0x801df88c --defsym ae_calc=0x801e4854 --defsym fa_apply=0x8011eaa0 --defsym fa_set_x=0x801263a8 --defsym fa_set_y=0x8012640c"
Native { & wsl.exe bash -c "cd '$wslRoot' && mipsel-linux-gnu-gcc $cflags -c src/osdmenu.c -o build/osd.o && mipsel-linux-gnu-ld $lflags -o build/osd.elf build/osd.o" }
CheckExit "compile/link"

# 2) splice + patch -------------------------------------------------------------------------------
Step "2/4 splice + patch"
$env:KODAK_RES = $Resolution
$env:KODAK_BR = $Bitrate
$env:KODAK_NOHOOKS = $(if($NoHooks){"1"}else{"0"})   # set every run so it can't leak from a previous one
if($Date -notmatch '^\d{4}-\d{2}-\d{2}$'){ Die "-Date must be YYYY-MM-DD" }
$env:KODAK_BUILD = "$Build"                           # boot splash: "Vers: FW <N> Type D  Date: <date>"
$env:KODAK_DATE = $Date
$env:KODAK_SPLASH = $(if($NoSplash){"0"}else{"1"})
Native { python "$Root\tools\osd_build.py" }
CheckExit "osd_build.py"

# 3) checksum + pack (checksum -> compress -> checksum) --------------------------------------------
Step "3/4 pack"
Push-Location "$Root\build"
try {
  Native { & "$Root\bin\ntkcalcVS.exe" -cw vd-osd.rbn           | Out-Null }; CheckExit "ntkcalc (raw)"
  Native { & "$Root\bin\bfc4ntkVS.exe" -c vd-osd.rbn vd-osd.bcl | Out-Null }; CheckExit "bfc4ntk (compress)"
  Native { & "$Root\bin\ntkcalcVS.exe" -cw vd-osd.bcl           | Out-Null }; CheckExit "ntkcalc (packed)"
} finally { Pop-Location }
Write-Host "packed build\vd-osd.bcl ($((Get-Item "$Root\build\vd-osd.bcl").Length) bytes)"

# 4) stage ----------------------------------------------------------------------------------------
Step "4/4 stage"
$out = "$Root\out\build-$Build"
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item "$Root\build\vd-osd.bcl" "$out\FWDV280.BIN" -Force
@(
  "Build=$Build"
  "Resolution=$Resolution"
  "NoHooks=$([bool]$NoHooks)"
  "Splash=$(if($NoSplash){"original"}else{"FW $Build, $Date"})"
  "SourceSHA256=$((Get-FileHash "$Root\src\osdmenu.c" -Algorithm SHA256).Hash)"
  "FirmwareSHA256=$((Get-FileHash "$out\FWDV280.BIN" -Algorithm SHA256).Hash)"
  "Date=$(Get-Date -Format o)"
) | Set-Content -Encoding utf8 "$out\manifest.txt"
Write-Host "staged -> $out\FWDV280.BIN"

# 5) patch-loader (optional) ----------------------------------------------------------------------
if($Card){
  Step "5/5 patch-loader (run from card)"
  if($Dump -and -not (Test-Path $Dump)){ Die "-Dump $Dump not found" }
  $pargs = @("$Root\tools\make_pldr.py", "--target-rbn", "$Root\build\vd-osd.rbn", "--target-bin", "$out\FWDV280.BIN",
             "--out", "$out\FW96658T.BIN", "--build", "$Build")
  if($Dump){ $pargs += @("--dump", $Dump) }
  $plog = Native { python @pargs }
  CheckExit "make_pldr.py"
  $plog | ForEach-Object { Write-Host $_ }
  @(
    "CardSHA256=$((Get-FileHash "$out\FW96658T.BIN" -Algorithm SHA256).Hash)"
    ($plog | Where-Object { $_ -like "PLDR_BASES=*" }) -replace '^PLDR_BASES=','CardBases='
    "CardEmulatorTest=$(if($Dump){"passed"}else{"skipped"})"
  ) | Add-Content -Encoding utf8 "$out\manifest.txt"
  Write-Host "staged -> $out\FW96658T.BIN"
}

if($Drive){
  if($Card){ & "$Root\deploy.ps1" -Bin "$out\FW96658T.BIN" -Drive $Drive -Card }
  else     { & "$Root\deploy.ps1" -Bin "$out\FWDV280.BIN" -Drive $Drive }
}
Write-Host "`nBUILD OK: FW $Build" -ForegroundColor Green
