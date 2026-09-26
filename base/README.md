# base/: videodoctor's Type D base firmware

The mod is spliced into **videodoctor's Type D build** of the Kodak Reels firmware. It is not
committed here. Download it yourself and drop it in this folder.

## Get it

videodoctor posted it as FWDV280.BIN in the "Hacking the Kodak Reels 8mm Film Digitizer" thread
on tinkerdifferent.com (post 225, January 21, 2026: https://tinkerdifferent.com/posts/43847/).

Save it here as **`base\FWDV280.BIN`**. On the first build, `build.ps1` unpacks it to
`base\vd-FWDV280-D.rbn` with `bin\bfc4ntkVS.exe -d`.

## Check it

| file | size | SHA256 |
|---|---|---|
| `FWDV280.BIN` (as downloaded, packed) | 3,938,896 | `4aba449dba13c5107094b346414c61aad8a73d28a24da827e7e0e46fadf107ec` |
| `vd-FWDV280-D.rbn` (unpacked) | 14,716,332 | `90d2abb68b1232b4c759c774105922888945a9250183c79f1213b37b0bb199c6` |

```powershell
Get-FileHash base\FWDV280.BIN -Algorithm SHA256
```

`tools/osd_build.py` refuses to build if the unpacked image's SHA256 is different.

## Why this exact file

Every address in the mod (hook sites, RAM variables, the free space it's linked into) belongs to this
one image. The patcher also asserts the original instruction at every patch site, so a wrong base fails
at build time and never produces firmware that hangs at boot.

Other base firmware will **not** work as is:

- **Stock Kodak Type D firmware**: different code layout, no 1600×1200 setup.
- **0dan0's `fw/FWDV280-D.rbn`**: the shared memory map is the same, but it has a different hook
  layer, and that layer freezes at boot on Type D.

Porting to another base is possible. Set `KODAK_ANYBASE=1` to skip the hash check, then fix whatever
the asserts complain about. See [../docs/TECHNICAL.md](../docs/TECHNICAL.md).

## Is my scanner a Type D?

Type D units shipped with Kodak firmware **v2.0**. If an A/B/C build from 0dan0 doesn't boot on your
unit, or shows wrong colours, it is probably a D. When in doubt, ask in the forum thread before
flashing. People there can usually tell the type from the serial number.
