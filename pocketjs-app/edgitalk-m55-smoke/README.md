# Beat Dash for Edgi-Talk M55

A two-lane rhythm game plus the home, calendar, settings and PC-monitor screens
for the Edgi-Talk's 400x240 logical viewport (rendered 2x onto the 800x480 panel,
30 Hz tick). It is packaged as a `.pocket` container (JavaScript + baked-font PAK)
and embedded in the M55 firmware.

How to play, pair the PC tool, build and flash is documented with the firmware
project: `projects/Edgi_Talk_M55_PocketJS/docs/README.md`.

## Layout

| Path | Purpose |
| --- | --- |
| `app.tsx` | Screen router. Hub pages are mounted on first visit and then only hidden; every hub page is dropped while a song plays. |
| `theme.ts`, `ui/` | Colours, type scale and shared components. |
| `native.ts` | Typed wrapper around the firmware `__edgi` bridge. |
| `screens/` | Home, Songs, Stage, Result, Settings, Calendar, PcMonitor. |
| `ring_*.png` | Gauge ring sprites (regenerate with `tools/assets/make_rings.py` in the firmware project). |
| `game/` | Built-in songs and charts, judgement logic. |
| `note_*.png` | Note sprites (single image ops keep the software renderer fast). |

## How to play

- Notes travel right to left on two lanes: air (blue) and ground (red).
- Tap the upper half of the screen for air, the lower half for ground.
- PERFECT within 90 ms, GREAT within 170 ms, otherwise MISS. Big notes give a bonus.
- Misses drain HP; ranks are S / A / B / C / F.
- Songs are synthesised on the board; notes follow the audio clock. If taps feel
  early or late, adjust **Audio offset** in Settings.

Best scores, the audio offset and the pairing code are stored in
`/flash/pocketjs_game.bin`.

## Wi-Fi and the PC companion

Once the board is on Wi-Fi it serves a small REST API (pairing code in
Settings, sent as `X-Token`) and answers UDP discovery on port 47808. The
companion uses it to read scores, upload a custom song (Songs -> "PC" card,
stored as `/flash/pocketjs_song.json`) and push PC stats for the monitor page
(`PUT /api/pc`). The date chip opens the calendar; today is a filled teal disc.

## Performance notes (software renderer)

The renderer diffs the draw list op by op; if the op sequence changes, the whole
screen repaints. Screens therefore keep the draw list structurally stable while
playing (notes are pooled image ops parked on the right edge, no empty text, no
transform-kind changes). Gauges are sprites for the same reason (a real arc changes the draw list on
every value change). Measured on hardware: Home about 24-30 fps, Stage about
22 fps during play.

## SD-card music

The home screen's player has a built-in demo melody. For SD-card music, put up to
16 mono, 16-bit PCM WAV files in `/sdcard/music` (16, 24, 48 or 96 kHz; no MP3,
no stereo). The left / right controls change tracks, the middle control
pauses or resumes, and `-` / `+` change volume. The card is scanned at startup and
periodically while playback is stopped. Game audio and the player share the
same output; the game takes over while a song is playing.
