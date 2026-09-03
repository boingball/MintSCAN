# MintSCAN artwork and release icons

The source artwork in this directory is kept separately from the binary
Workbench DiskObjects used by the Amiga release.

Release icons:

- `MintScan.info` - application Workbench tool icon (`WBTOOL`, type 3).
- `MintSCAN.info` - release drawer icon (`WBDRAWER`, type 2).
- `Install.info` - Installer project icon (`WBPROJECT`, type 4).
- `MintSCANFolder.info` - alternate folder/drawer artwork
  (`WBDRAWER`, type 2), retained as source art.

Source PNG artwork:

- `MintScan.png` and `MintScan-clicked.png` - application states.
- `mint-scan.png` and `mint-scan-clicked.png` - alternate application states.
- `folder-closed.png` and `folder-open.png` - drawer states.
- `Installer.png` - installer artwork.

`make release` validates the three release icon types, checks that the
application icon requests a 131072-byte stack, and rejects byte-identical
application/drawer icons. It copies only the correctly named release icons
into their Amiga locations:

- `release/Install.info` beside `release/Install`.
- `release/MintSCAN.info` beside `release/MintSCAN/`.
- `release/MintSCAN/MintScan.info` beside `MintScan`.

The top-level `GNUmakefile` adjusts only the packaged drawer's saved window
height to 120 pixels; source artwork remains untouched.
