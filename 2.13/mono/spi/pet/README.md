# Open-EP Pet

A Tamagotchi-style virtual pet running on the 2.13" mono PixPaper e-paper
panel over SPI, animated with partial
refresh so the puppy blinks, walks, eats and jumps without full-screen
flashing.

The UI shows the pet sprite, three stat bars (空腹 hunger / 元気 energy /
気分 mood) and three action buttons (エサ feed / あそぶ play / そうじ clean),
in either landscape or portrait orientation.

## Layout

| File | Description |
|---|---|
| `pet_app.h` | Board-independent core: SPI transport, EPD init/partial-refresh sequence, custom partial LUT, framebuffer drawing, Latin + Japanese fonts, pet state machine and main loop |
| `pixpaper-213-m-test-frdm-imx93-pet.c` | GPIO backend for FRDM-i.MX93 (libgpiod **v1**) |
| `pixpaper-213-m-test-rpi5-pet.c` | GPIO backend for Raspberry Pi 5 (libgpiod **v2**) |
| `petsprite.h` | Generated 1-bit sprite frames (do not edit) |
| `jpfont.h` | Generated 1-bit Japanese glyph bitmaps (do not edit) |
| `sprite_gen.py` | Regenerates `petsprite.h` from the `dog*.png` artwork |
| `jpfont_gen.py` | Regenerates `jpfont.h` from a system Noto CJK font |
| `dog*.png` | Source artwork for the 14 sprite frames |
| `pixpaper-pet.service` | systemd unit to run the pet as an auto demo at boot |
| `Makefile` | Local build for both targets |

Each per-board `.c` file only provides the GPIO backend
(`epd_dc_set` / `epd_rst_set` / `epd_busy_get` / `epd_gpio_init` /
`epd_gpio_release`) and then includes `pet_app.h`, which contains
everything else including `main()`.

## Hardware / wiring

SPI device: `/dev/spidev0.0` at 5 MHz, mode 0.

| Signal | FRDM-i.MX93 (`gpiochip0`) | Raspberry Pi 5 (`gpiochip15`) |
|---|---|---|
| DC | line 0 | line 5 |
| RST | line 5 | line 6 |
| BUSY | line 26 | line 26 |

To port to another board, copy one of the `.c` files and adapt the chip
name / line offsets (use the libgpiod v1 file for libgpiod < 2.0, the
rpi5 file for v2).

## Build

Requires libgpiod development headers (v1 for the imx93 target, v2 for
the rpi5 target).

```sh
make            # build every target
make imx93      # FRDM-i.MX93 (libgpiod v1)
make rpi5       # Raspberry Pi 5 (libgpiod v2)
make clean
```

## Run

```sh
sudo ./pixpaper-213-m-test-rpi5-pet [auto|interactive] [landscape|portrait]
```

- **Orientation**: `landscape` (default) or `portrait`. If portrait comes
  out upside-down on your unit, see the note at the top of `pet_app.h`.
- **Interactive mode** (default when stdin is a TTY) — keys:
  - `f` feed (エサ): +hunger
  - `p` play (あそぶ): +mood, -energy
  - `c` clean (そうじ): +mood
  - `q` quit
- **Auto mode** (`auto`, or automatic when stdin is not a TTY, e.g. under
  systemd): cycles feed → play → clean on its own as a demo.

Left alone, the pet idles with random liveliness gestures (blinks,
looking around, a short walk cycle, a floating heart), falls asleep after
~60 frames without interaction, and its stats slowly decay; at zero
hunger it sulks until fed.

## Regenerating the assets

Both generated headers are committed, so this is only needed after
changing the artwork or UI strings. Requires Python 3 with Pillow and
NumPy.

```sh
python3 sprite_gen.py --emit   # dog*.png -> petsprite.h (preview: /tmp/sprite_frames.png)
python3 jpfont_gen.py          # Noto Sans CJK Bold -> jpfont.h
```

`sprite_gen.py` traces every frame at one fixed scale (anchored to the
sitting frame) and bottom-aligns them on a common floor, so crouch/jump
poses keep their real height differences. `jpfont_gen.py` embeds only
the glyphs actually used by the UI (edit its `JOBS` list when adding
strings).

## Display technique notes

- **Partial refresh** uses a custom LUT (`WF_PARTIAL` in `pet_app.h`);
  animation frames are written to RAM 0x24 only, keeping the panel in a
  `0x0C` "keep on" display mode between frames (~35 ms/frame pacing).
- **Ghosting control**: every 200 frames the panel is hardware-reset,
  re-initialised and given a full refresh of the current frame as a new
  base map.
- **Blink masking**: when switching between sprite frames with very
  different silhouettes (sad/eat/walk), a one-frame blink pose is
  inserted in between to hide partial-refresh residue.
