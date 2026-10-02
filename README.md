# Clipper

Tiny replay-buffer game clipper for Windows. Press a hotkey and the last N seconds are saved as an MP4 under a file size cap (default 19 MB, so it fits Discord's upload limit).

- **Video:** Desktop Duplication → GPU scale/convert (D3D11 video processor) → hardware H.264 (Media Foundation). HDR displays are tone-mapped. Variable frame rate: only frames the screen actually shows are recorded, so a game dipping to 5 fps records 5 fps instead of repeats.
- **Audio:** WASAPI game-only process loopback, Discord, mic, or full desktop → mixer → AAC.
- Encoded packets sit in RAM; saving is a remux, recompressed only if needed to land under the size cap.
- Records only while the configured game is running, on the monitor it's on.
- Single `.cpp`, no dependencies beyond the Windows SDK.

## Build

Requires Visual Studio (any edition) with the C++ desktop workload.

```
build.bat
```

Produces `clipper.exe`. Set `VERSION` (e.g. `set VERSION=1.2.0`) before building to stamp a version into the window title; otherwise it shows `dev`.

## Releases

Every push to `main` builds `clipper.exe` on GitHub Actions, runs `--selftest`, tags the next `vX.Y.Z` and publishes it as a GitHub release. The bump is decided from commit messages since the last tag: `#major` or `BREAKING CHANGE` → major, `#minor` or `feat:` → minor, otherwise patch. Put `#skip-release` in the head commit message to skip a release.

## Use

Run `clipper.exe`. It lives in the tray: right-click for **Settings**, **Save clip now**, **Open clips folder**.

Default hotkey is **F9**. Clips go to `Videos\Clips`.

## Config

On first run a commented `clipper.ini` is written next to the exe. Edit it through the Settings window, or by hand and restart Clipper.

| Key | Default | Meaning |
|---|---|---|
| `hotkey` | `F9` | Save hotkey, e.g. `Alt+F10`, `Ctrl+Shift+S` |
| `game` | `Discovery.exe` | Exe to watch. Empty = always record the primary monitor |
| `audio` | `game+discord+mic` | Any of `game`, `discord`, `mic`, `desktop` joined with `+`, or `none` |
| `gamevol` / `discordvol` / `micvol` | `1.0` | Volume multipliers |
| `seconds` | `30` | Clip length |
| `height` | `0` | Output height, `0` = best for the size cap |
| `fps` | `60` | Frame rate |
| `crop` | `0` | `1` = crop ultrawide to centre 16:9 |
| `maxmb` | `19` | Hard file size cap in MB |
| `bitrate` | `0` | Video kbps while recording. `0` = auto from `maxmb`; a fixed rate saves clips under the cap instantly |
| `audiokbps` | `128` | AAC bitrate: `96`, `128`, `160` or `192` |
| `mono` | `0` | `1` = mix audio down to one channel |
| `folder` | *(empty)* | Clip folder, empty = `Videos\Clips` |

## License

MIT
