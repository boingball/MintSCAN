# MintSCAN artwork checklist

Replace the existing placeholder files before building the 1.1.0 release.

Required release icons:

- `art/MintScan.info` - Workbench tool icon (`do_Type=3`) for the application.
- `art/MintSCAN.info` - Workbench drawer icon (`do_Type=2`) for the archive drawer.
- `art/Install.info` - Workbench project icon (`do_Type=4`) whose default tool is `Installer`.

Set the MintScan tool icon's Workbench **Stack** value to exactly **131072**
bytes. This matches the application's `__stack` and `$STACK:` request.

Source artwork may also be kept here using the MintAMP/MintPRINT convention,
for example:

- `MintScan.png` and `MintScan-clicked.png`
- `folder-closed.png` and `folder-open.png`
- `Install.png` and `Install-selected.png`

`make release` verifies the DiskObject types, stack value, and that the
application and drawer icons are not byte-identical. It deliberately fails
with the current placeholders so they cannot accidentally reach Aminet.
