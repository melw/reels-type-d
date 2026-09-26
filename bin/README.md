# bin/: Novatek packing tools

The build needs two Windows command-line tools from the Novatek SDK tool chain. They are not committed
here. Copy them from 0dan0's repo (https://github.com/0dan0/4reels), where both are in `utils/` and
in `fw/`:

| file | used for |
|---|---|
| `ntkcalcVS.exe` | writes the firmware checksum (`-cw`), before and after compression |
| `bfc4ntkVS.exe` | compresses the raw image into the flashable `FWDV280.BIN` (`-c`), and unpacks the base (`-d`) |

Put both directly in this folder (`bin\ntkcalcVS.exe`, `bin\bfc4ntkVS.exe`).

## Antivirus

Some antivirus products quarantine or kill `bfc4ntkVS.exe` because it is an unsigned packer.
If the pack step fails or the file disappears, check your antivirus settings to allow it.
