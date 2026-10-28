# System Shock's data

A commercial game, so you need your own copy: the **DOS CD version**. Put
its `SSHOCK` directory here, as `data/SSHOCK/`, holding `DATA/` and
`SOUND/`. A symbolic link works, e.g. to the copy Steam installs with
*System Shock: Classic*:

```
ln -s ~/.local/share/Steam/steamapps/common/SS1/SSHOCK data/SSHOCK
```

Tested with the CD version as Steam installs it:

| File | Size | MD5 |
|---|---|---|
| `SSHOCK/DATA/CITMAT.RES` | 105,834 | `e8727598d54c360eff5916b9b1570082` |
| `SSHOCK/DATA/ARCHIVE.DAT` | 613,566 | `d290ef2e3c4fe605473d7cf0b65a96dc` |

Other versions get a warning and may work. Only the English files and the
low-resolution movies go on the card (`tools/put_data.sh`); the music is
rendered from `SOUND/` (`tools/make_music.sh`).
