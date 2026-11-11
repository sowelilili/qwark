# The installer package

`qwark-installer.pkg` installs qwark on a console with no PC. It is a small homebrew app, **qwark installer (build N)**, title id `QWRK00001`, that carries a `qwark.sprx` and the `ui.pak` built with it, and RaCMAN Reloaded's mods and premade savefiles. It runs on CFW with Cobra and on PS3HEN.

## Using it

1. Put `qwark-installer.pkg` on a USB drive, or download it with the console's browser. On PS3HEN, enable HEN first.
2. Install it with **Package Manager**, **Install Package Files**.
3. Start **qwark installer** from the Game column. It asks once before it writes anything.
4. It says what it did and offers to restart. A boot plugin only loads when the console starts. On PS3HEN, enable HEN again after the restart; qwark loads with it.

The app can stay on the console. Running it again is safe: the same files go in again, a list that already starts with qwark is left alone, and mods and savefiles that are already there are skipped. Delete it from the Game column like any other app.

## What it does

The same as the PC client's **Install to boot_plugins.txt**:

| | |
|---|---|
| `/dev_hdd0/plugins/qwark.sprx` | the module |
| `/dev_hdd0/qwark/ui.pak` | the console menu's renderers and, since build 56, the savefile helpers (and Tools of Destruction's Lua helper, revision 1.29: a test build's and, since build 57, a release's), which must come from the same build |
| `/dev_hdd0/qwark/rac1.sprx` .. `rac4.sprx`, `tod.sprx` | the game modules, each named for its game (PROTOCOL.md 9.11), which a test build's package carries and, since build 57, a release's too (build 56's release carried none); they go in before the module, and one that is not a signed module stops the install before anything is written. Modules already on the console that the package does not carry are left alone |
| `/dev_hdd0/boot_plugins.txt` | qwark's line goes first, so it loads before webMAN; every other line stays, in its order |
| `/dev_hdd0/qwark/boot_plugins.txt.bak` | the list as it was, whenever the installer changes it |
| `/dev_hdd0/plugins/qwark.sprx.old` | the module that was there before, if there was one |

The list is edited by the client's rules (`installer/bootlist.h`): a line naming `qwark.sprx` anywhere else, the old `/dev_hdd0/tmp` path included, is dropped, so qwark is named once; CRLF comes back as LF and blank lines go. A list that already starts with qwark's line is not touched.

It also says so when:

- the list has more than six plugins. Cobra and PS3HEN load six at most, so the last ones do not load. Nothing is dropped; only you can say which;
- there is a `boot_plugins.txt` on a USB drive. The console reads that one instead, so qwark will not load until it goes.

It never edits `boot_plugins_nocobra.txt`, `mamba_plugins.txt` or a USB list, as the client does not.

A failure leaves the console as it was. (The mods and savefiles below are the exception: they go in after qwark, and their own way.) The installer writes every new file as `<name>.tmp` beside its place and reads each one back first. Then it moves them in, the backup first and the list last, each time renaming the old file to `<name>.old` until all are in, and it puts them all back if one move fails. The message says which step failed. A power cut can at worst leave no list for a moment, which boots without plugins; the next run takes the list from `boot_plugins.txt.old`.

The restart is lv2's quick restart, the call webMAN MOD's own installer makes. If it does not happen, the app says so; restart from the XMB.

### Mods and premade savefiles

The package also carries the mods and premade savefiles the PC client ships, so a console with no PC has them too. The installer puts them where the client's own uploads put them, so qwark finds them as if the client had sent them:

| | |
|---|---|
| `/dev_hdd0/qwark/mods/<TITLEID>/<mod>/` | each mod the client would upload: `patch.txt`, the `.bin` files it names, and `qwark.sum`, the client's hash of them. The rest of the client's mod folder (sources, notes, `src.zip`) stays on the PC, as it does when the client uploads a mod. The client's `mods/libs/` (the old client's Lua helpers) is not carried: the client never uploads it and qwark never reads it |
| `/dev_hdd0/qwark/savefiles/<TITLEID>/(premade savefiles)/<name>.sav` | each premade savefile, with `<name>.sav.sum`, the category a load of a premade savefile uploads into. The other title ids of the client's `premade-savefiles/titles.txt` get their copies too: RaC2's two repacks under `(premade savefiles)`, and the disc trilogy `BCES01503` under `(premade savefiles) rac1`, `rac2` and `rac3` |

Only what is missing goes in. A mod whose folder is already there, or a savefile that is already there, is skipped and never replaced, whoever put it there and whatever is in it: a mod the client uploaded, one you edited, or a save you stored over a premade one all stay exactly as they are. The message counts what was added and what was skipped ("Skipped 2 mods and 12 savefiles that were already installed."). To get a newer copy of a shipped mod or savefile, delete yours first, or let the PC client send it.

They go in after qwark itself, so nothing about them can stop qwark from installing. Each one goes in whole or not at all: every file is checked against the CRC-32 the package gives it before anything is written, and one that does not match is not written; a mod is written into `/dev_hdd0/qwark/staging/` and moved into place in one step, and a savefile is written as `<name>.sav.tmp`, read back and renamed. If one cannot be added, the others still are, and the message says qwark is installed and how many were not added, and why.

## Undoing it

- Remove qwark's line from `/dev_hdd0/boot_plugins.txt`, with the client's **Remove from boot_plugins.txt**, webMAN's file manager or FTP, or copy `/dev_hdd0/qwark/boot_plugins.txt.bak` back over it. Then restart.
- Delete `/dev_hdd0/plugins/qwark.sprx` and the `/dev_hdd0/qwark` folder to remove qwark's files and settings.
- If the console does not reach the XMB after an install: on PS3HEN, hold R2 while HEN enables, which skips the boot plugins. On Cobra CFW, start in Recovery Mode, which takes `boot_plugins.txt` out of the way (Evilnat's Cobra renames it to `boot_plugins.txt.bak`). Then fix the list as above.

## How it is built

`make pkg` makes it from the `qwark.sprx` and `ui.pak` beside the makefile, and any of `rac1.sprx` to `rac4.sprx` and `tod.sprx` there (the game modules, `make games`, which `make` and `make release` both build), so run `make` first; `make dist` makes it too and copies it to `dist/`. The mods and premade savefiles come from the client's checkout, `CLIENT_DIR`, `../racman-reloaded` by default (`make pkg CLIENT_DIR=../wt-client` for another one; `CLIENT_DIR=` with nothing makes a package without them). The committed `dist/qwark-installer.pkg` carries them, so the client's release takes the package from `dist/` as before and needs no SDK. The parts:

- `installer/main.c`: the app. A plain screen of its own, which a system message dialog needs to be drawn on, the dialogs, and the restart.
- `installer/install.c` and `installer/bootlist.c`: the install and the list edit, over a small file interface (`ifs.h`) that is CellFs on the console (`ifs_ps3.c`) and a folder on the PC for the tests. `install.h` describes the library of mods and savefiles and its file, `USRDIR/library.bin`.
- `installer/qrle.c`: the expansion of the savefiles. A PS3 package is not compressed, and the premade savefiles are 8.7 MB, almost all of it runs of one byte (zero padding, and Deadlocked's 0xFF fill). The SDK has no zlib a PPU program can link (only Edge's, which runs on the SPUs), so the package stores each file in a run-length code of its own (`qrle.h` describes it; about 200 KB for all of them, with the mods), or as it is when that is shorter, and files with the same bytes share them.
- `installer/makefile`: the ELF, with the SDK's own rules. The top-level makefile strips it and signs it with `scetool` as a free NPDRM `EBOOT.BIN`, with the 3.55 keys (key revision 0A), which is how homebrew for PS3HEN is signed.
- `installer/pkg.py`: the library (`library.bin`), made from the client's checkout by the client's own rules: the mod folders its `publish.ps1` ships (`$ExpectedMods`, less `libs`), less the mods it drops (an `automation:` line) or never offers (`#- visible: false`), each as `ModLibrary` uploads it; and every premade savefile, with `titles.txt`'s other title ids, as `PremadeSaveFiles` names their console categories. `pkg.py check` expands every file of the package's library, holds each to its CRC and to the client's own file, and fails when the checkout has changed since. Then `PARAM.SFO` (the build is in `TITLE` and in `APP_VER`, 00.55 for build 55), `ICON0.PNG`, and the package. The icon on the XMB is RaCMAN Reloaded's: `installer/icon0.png`, 320 x 176 (the size of an `ICON0.PNG`), 8-bit RGBA, about 35 KB. The script only reads it, after checking that it is a whole PNG of that size, because the build has no image library. It was made once from the client's `packaging/racman-reloaded.png` (256 x 256), fitted inside the 320 x 176 and centred without distortion, so 176 x 176 on a transparent background: PowerShell's .NET `System.Drawing`, `Graphics.DrawImage` with `HighQualityBicubic` onto a transparent 320 x 176 `Bitmap`, saved as PNG, then its `gAMA`, `sRGB` and `pHYs` chunks taken out so the console's decoder has only pixels to read. To change it, do that again and replace the file.

The package is the SDK's debug package format, which Package Manager installs on CFW and PS3HEN. The SDK's own `make_package_npdrm` cannot make it: it takes only an `EBOOT.BIN` from `make_fself_npdrm`, which it signs as a debug program that PS3HEN cannot run, and no SELF that is not an NPDRM one, which `qwark.sprx` is not. So `pkg.py` packs it. Its check has the SDK's tool read, check and list the package; the only complaints left are those two rules. It also has the SDK pack the same files its own way and reproduces that package byte for byte. It never asks the SDK to unpack the package: `make_package_npdrm --extract` never finishes an `EBOOT.BIN` signed by scetool, and fills the disk while it tries.

`installer/test/run.sh`, which `test/run.sh` runs, tests the list edit and the install over a fake HDD, with a failure put at every file call in turn, the library's install the same way (only what is missing goes in, a file that does not match its CRC is not written, every failure leaves each mod and savefile whole or absent), and qrle; then `installer/test/test_pkgdata.py` holds `pkg.py`'s library to the client's rules, its encoder to `qrle.c`, and installs a library it made with the console's own code onto a fake HDD. What only a console can tell is in the hardware test list of the change that added the installer.
