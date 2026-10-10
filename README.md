# MiiFlood - bulk Mii Parade importer (Wii homebrew)

Merges Miis into the Wii Mii Parade (the hidden database inside
`/shared2/menu/FaceLib/RFL_DB.dat`, 10,000 slots), keeps an unlimited archive on
your SD card, and backs up `RFL_DB.dat` before every change.

## Get the .dol (no compiler needed)
1. Make a free GitHub account, create a new repository, upload this whole folder (keep `.github/`).
2. Open the **Actions** tab -> "Build MiiFlood" -> when green, download the **MiiFlood** artifact.
3. Unzip it onto the SD card root so you have `sd:/apps/MiiFlood/boot.dol`.
(Or with devkitPro installed: `make`, then copy `boot.dol`, `meta.xml`, `icon.png` to `sd:/apps/MiiFlood/`.)

## Use
1. Put Miis in `sd:/apps/MiiFlood/import/` (subfolders are fine):
   - `.mii` single Mii (74 or 76 bytes)
   - `.miis` / `.bin` bundle of 74-byte Miis back to back
   - a whole `RFL_DB.dat` from another Wii/Dolphin (imports its plaza and parade Miis)
2. Launch MiiFlood from the Homebrew Channel. Press **A** to import. It scans, tells you how
   many new Miis it found, asks to confirm, makes a verified backup, writes, then re-reads NAND to verify.
3. Press **1** any time to append the whole current parade to `archive/parade_archive.miis`.
   The archive is re-read on every import, so Miis that get pushed out of the 10,000 slots come back later.
4. Press **2** to restore the newest backup.

Options: **+** replace oldest when full; **-** include "private" (mingle-off) Miis.

## Safety
- Refuses to edit unless size, headers, list structure and checksum all match what the Mii Channel writes.
- Backup to SD must verify before NAND is touched; failed write/verify automatically restores the original.
- Official (plaza) Miis are never modified. Miis already in your plaza or parade are skipped.
- Needs NAND write access: launch from the Homebrew Channel on a modded Wii (IOS with ISFS permissions).

## Status
Core logic (`source/rflcore.c`) is unit-tested (`tests/`): CRC detection, list integrity, eviction,
dedupe, filters, and an independent Python re-parse. The format comes from community decompilation
of Nintendo's RFL library and has NOT been run on real hardware. Test with a NAND backup first.
