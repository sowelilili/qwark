# The installer package

`qwark-installer.pkg` installs qwark on a console with no PC. It is a small homebrew app, **qwark installer (build N)**, title id `QWRK00001`, that carries a `qwark.sprx` and the `ui.pak` built with it. It runs on CFW with Cobra and on PS3HEN.

## Using it

1. Put `qwark-installer.pkg` on a USB drive, or download it with the console's browser. On PS3HEN, enable HEN first.
2. Install it with **Package Manager**, **Install Package Files**.
3. Start **qwark installer** from the Game column. It asks once before it writes anything.
4. It says what it did and offers to restart. A boot plugin only loads when the console starts. On PS3HEN, enable HEN again after the restart; qwark loads with it.

The app can stay on the console. Running it again is safe: the same files go in again, and a list that already starts with qwark is left alone. Delete it from the Game column like any other app.

## What it does

The same as the PC client's **Install to boot_plugins.txt**:

| | |
|---|---|
| `/dev_hdd0/plugins/qwark.sprx` | the module |
| `/dev_hdd0/qwark/ui.pak` | the console menu's file, which must come from the same build |
| `/dev_hdd0/boot_plugins.txt` | qwark's line goes first, so it loads before webMAN; every other line stays, in its order |
| `/dev_hdd0/qwark/boot_plugins.txt.bak` | the list as it was, whenever the installer changes it |
| `/dev_hdd0/plugins/qwark.sprx.old` | the module that was there before, if there was one |

The list is edited by the client's rules (`installer/bootlist.h`): a line naming `qwark.sprx` anywhere else, the old `/dev_hdd0/tmp` path included, is dropped, so qwark is named once; CRLF comes back as LF and blank lines go. A list that already starts with qwark's line is not touched.

It also says so when:

- the list has more than six plugins. Cobra and PS3HEN load six at most, so the last ones do not load. Nothing is dropped; only you can say which;
- there is a `boot_plugins.txt` on a USB drive. The console reads that one instead, so qwark will not load until it goes.

It never edits `boot_plugins_nocobra.txt`, `mamba_plugins.txt` or a USB list, as the client does not.

A failure leaves the console as it was. The installer writes every new file as `<name>.tmp` beside its place and reads each one back first. Then it moves them in, the backup first and the list last, each time renaming the old file to `<name>.old` until all are in, and it puts them all back if one move fails. The message says which step failed. A power cut can at worst leave no list for a moment, which boots without plugins; the next run takes the list from `boot_plugins.txt.old`.

The restart is lv2's quick restart, the call webMAN MOD's own installer makes. If it does not happen, the app says so; restart from the XMB.

## Undoing it

- Remove qwark's line from `/dev_hdd0/boot_plugins.txt`, with the client's **Remove from boot_plugins.txt**, webMAN's file manager or FTP, or copy `/dev_hdd0/qwark/boot_plugins.txt.bak` back over it. Then restart.
- Delete `/dev_hdd0/plugins/qwark.sprx` and the `/dev_hdd0/qwark` folder to remove qwark's files and settings.
- If the console does not reach the XMB after an install: on PS3HEN, hold R2 while HEN enables, which skips the boot plugins. On Cobra CFW, start in Recovery Mode, which takes `boot_plugins.txt` out of the way (Evilnat's Cobra renames it to `boot_plugins.txt.bak`). Then fix the list as above.

## How it is built

`make pkg` makes it from the `qwark.sprx` and `ui.pak` beside the makefile, so run `make` first; `make dist` makes it too and copies it to `dist/`. The parts:

- `installer/main.c`: the app. A plain screen of its own, which a system message dialog needs to be drawn on, the dialogs, and the restart.
- `installer/install.c` and `installer/bootlist.c`: the install and the list edit, over a small file interface (`ifs.h`) that is CellFs on the console (`ifs_ps3.c`) and a folder on the PC for the tests.
- `installer/makefile`: the ELF, with the SDK's own rules. The top-level makefile strips it and signs it with `scetool` as a free NPDRM `EBOOT.BIN`, with the 3.55 keys (key revision 0A), which is how homebrew for PS3HEN is signed.
- `installer/pkg.py`: `PARAM.SFO` (the build is in `TITLE` and in `APP_VER`, 00.55 for build 55), `ICON0.PNG`, and the package. The icon on the XMB is RaCMAN Reloaded's: `installer/icon0.png`, 320 x 176 (the size of an `ICON0.PNG`), 8-bit RGBA, about 35 KB. The script only reads it, after checking that it is a whole PNG of that size, because the build has no image library. It was made once from the client's `packaging/racman-reloaded.png` (256 x 256), fitted inside the 320 x 176 and centred without distortion, so 176 x 176 on a transparent background: PowerShell's .NET `System.Drawing`, `Graphics.DrawImage` with `HighQualityBicubic` onto a transparent 320 x 176 `Bitmap`, saved as PNG, then its `gAMA`, `sRGB` and `pHYs` chunks taken out so the console's decoder has only pixels to read. To change it, do that again and replace the file.

The package is the SDK's debug package format, which Package Manager installs on CFW and PS3HEN. The SDK's own `make_package_npdrm` cannot make it: it takes only an `EBOOT.BIN` from `make_fself_npdrm`, which it signs as a debug program that PS3HEN cannot run, and no SELF that is not an NPDRM one, which `qwark.sprx` is not. So `pkg.py` packs it. Its check has the SDK's tool read, check and list the package; the only complaints left are those two rules. It also has the SDK pack the same files its own way and reproduces that package byte for byte. It never asks the SDK to unpack the package: `make_package_npdrm --extract` never finishes an `EBOOT.BIN` signed by scetool, and fills the disk while it tries.

`installer/test/run.sh`, which `test/run.sh` runs, tests the list edit and the install over a fake HDD, with a failure put at every file call in turn. What only a console can tell is in the hardware test list of the change that added the installer.
