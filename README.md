# COS-102 controller firmware — ESP32-S31 (PlatformIO + ESP-IDF 6.1)

Drives the Delta COS-102 LED sign (5 boards, 200×30 RGB) from an
**ESP32-S31-Function-CoreBoard-1** with the **COS-102 hat**
(`../cos102_s31_hat`). It is a port of the ESP32-S3 firmware in
`../LEDSIGNPART3/firmware`. The protocol, pixel mapping, effects and web UI are
the same; the hardware layer is rewritten for the S31.

Protocol reference: `../LEDSIGNPART3/docs/PROTOCOL.md`. How effects and text
animations work: `../LEDSIGNPART3/docs/DEVELOPING.md` (still accurate; the
file layout here is the same).

## Build, flash, monitor

PlatformIO's official `espressif32` platform doesn't support the S31 yet, so
`platformio.ini` pins the **pioarduino** platform release `61.04.00`
(ESP-IDF 6.1.1), which includes the `esp32-s31-coreboard-1` board.

Plug a USB-C cable into the CoreBoard's **USB-to-UART** port (the console is
on UART0, GPIO58/59). Then:

```bash
pio run -t upload
```

```bash
pio device monitor
```

The web UI (`src/web/index.html`) is turned into `src/web_index.c` by
`tools/gen_web.py`. That runs automatically before every build, so you just
edit the HTML and rebuild.

If the upload doesn't start, hold **BOOT**, tap **RST**, release BOOT, and
upload again. The hat has 3.2 mm holes over both buttons.

## First run: WiFi

At the `cos102>` prompt:

```
wifi YourNetwork YourPassword
```

It prints the IP when connected. Open `http://<ip>/`. With no stored
credentials, or if the network can't be reached, the sign starts an open
access point named **COS102-Setup-XXXX**. Join it and open
`http://192.168.4.1` to enter your WiFi details. Press **Save to flash** in the
UI to keep the current look across reboots.

## Web UI

- **Animation**: the start page. Pick an animation from the dropdown (‹ › step
  through them), with optional auto-cycle.
- **Text**: message, 14 text animations, size, colours.
- **Clock**: NTP time, 12/24 h, seconds, timezone.
- **Settings**: frame rate, colours, WiFi (with network scan), save/reboot.
- **Advanced**: the full S3 diagnostics tab. Backends, bus-clock tuning, test
  patterns, protocol probes (`ws`/`ww`/`wb`), protocol settings, mapping, and
  a terminal that runs any serial-console command.

REST API (same as the S3): `GET/POST /api/state`, `POST /api/action`
{play, pause, toggle, next, prev, save, blank, defaults, reboot},
`POST /api/wifi`, `GET /api/scan`, `POST /api/cmd` {"cmd": "..."},
WebSocket `/ws` (state push every 2 s).

## DJ mode: Mixxx beat sync and sample texts

The sign listens on **UDP 4210** for beat reports from the Mixxx bridge in
`../mixxx/` (setup: `../mixxx/README.md`). It also answers as
**cos102.local** (mDNS).

- `beatclock.c`: local beat clock locked to the reports. It trusts the
  least-delayed packets and ignores single late ones. In simulation it showed
  under 1 ms rms jitter on typical WiFi and re-locked within 200 ms after a
  cue jump.
- `beatsync.c`: UDP listener and per-frame beat position (`g_bf`). It looks
  two frames ahead plus the latency offset, so a flash lands when the frame
  is actually on the panel.
- 18 beat-synced animations free-run at 120 BPM without Mixxx: *Beat Flash,
  Strobe, Colors, Chase, Ripple, Bars, Bar Wipe, VU Meter, Ripple Rain,
  Stereo Ripple, Ripple Tunnel, Starburst, Kaleidoscope, Laser Fan, Beat
  Bounce, Beat Scanner, Beat Plasma, Sixteenths*. The newer ones are computed
  entirely from the beat position (no per-frame state), so they stay locked
  through cue jumps and tempo changes.
- `GET /api/frame` returns one snapshot of the panel (200×30 binary PPM),
  for checking effects remotely. Nothing streams. Settings are in the
  **🎧 Beat** tab: latency offset, beat pulse on any animation, change
  animation every N bars or on deck change, and tempo lock.
- `samples.c`: per-sampler texts shown while that Mixxx sampler plays
  (**🔊 Samples** tab, with live pad detection and a ▶ preview per row).
  Ten styles: Solid, Beat flash, Rainbow, Strobe, Marquee, Letter by
  letter, Drop in, Punch, Wave and Glitch. The intros start from the moment
  the pad is hit.

## Horses & Party

`effects_theme.c` holds five themed effects with pixel-art sprites written as
text grids (one character per pixel), so they're easy to edit by eye:
*Galloping Horses* (stride locked to the beat), *Carousel*, *Birthday Cake*,
*Balloons* and *Party Banner*.

## 3D & Shapes

`effects_art.c` has eleven effects in their own dropdown group: *Retro Sun,
Platonic Solids, Tesseract* (4D), *Donuts* (shaded, z-buffered), *Disco Ball,
Demo Tunnel, Wave Mesh, Julia Morph, Metaballs, Spirograph, Hyperspace*.
Wireframes use anti-aliased additive lines, so slow rotations glide. Per-frame
render cost was measured on the sign: the heaviest (Donuts, Julia, Metaballs)
take 6–8 ms, comfortably inside the frame at 60 fps. At 78 fps (12.8 ms)
there's still margin, but less.

Beat and sample settings are stored as their own NVS records, so adding them
didn't reset existing settings or WiFi.

## Motion: frame-locked, sharp edges

Effects with a travelling hard edge (Comet, Scanner, Wipe, Theater Chase)
and all moving text advance a **whole number of pixels on a fixed frame
cadence**: n px every frame, or 1 px every n frames (`motion_advance()` in
`effects.c`). A position computed from the clock (e.g. 40 px/s at 78 fps =
0.51 px/frame) steps 1,0,1,0,1,1,0… and that uneven rhythm is visible as a
pulsing stutter. The S3 version hid it with sub-pixel blending, which blurred
the text. Now text is sharp and the motion is even.

Trade-off: speeds snap to what the frame rate divides into. At 78 fps
that's 78, 39, 26, 19.5… px/s, so the speed slider moves in those steps.
Lower frame rates give other steps (60 fps: 60, 30, 20… px/s).

Simulated effects (Fire, Starfield, Twinkle, Sparkle, Confetti, Bouncing
Balls, Snow, Fireworks, Radar, Meteor Shower) were tuned per frame at 30 fps,
so on the S3 their speed followed the frame rate and ignored the speed slider
and pause. Each frame now advances them by `s_k = 30 / fps × speed / 50`
reference frames (fades compounded, spawns and physics scaled; see
`fade_k()`, `ticks()`, `chance()` in `effects.c`). The speed slider and pause
control them, and they look the same at any frame rate.

Frame pacing uses a **constant** number of scan cycles per frame
(`scanout_par_cycle_us()` is the nominal buffer length, never a live
measurement). A cycle count that changes between vsyncs changes the drivers'
PWM truncation, which shows up as brightness pulsing.

## What changed for the ESP32-S31

| Area | ESP32-S3 version | ESP32-S31 version |
|---|---|---|
| CPU | Xtensa LX7, 240 MHz | RISC-V, 320 MHz, dual core |
| Panel GPIOs | 4–8, 15–18 (bank 0) | 39, 40, 42–48 (bank 1; see `src/pins.h`) |
| Bit-bang registers | `GPIO_OUT_W1TS/W1TC` | `GPIO_OUT1_W1TS/W1TC` |
| Scan engine (GCLK + A/B/C) | LCD_CAM i80 DMA, 8-bit bus + 6 dummy pins | **Parallel IO (PARLIO) TX DMA**, 4-bit bus, no dummy pins |
| GCLK steps | any pclk the LCD divider gives | 160 MHz / n (… 26.7, **20**, 16, 13.3 … MHz) |
| Data path | SPI2 quad DMA + GPIO latch | same; IO3 uses spare GPIO49 only during init |
| Console | USB-Serial-JTAG | UART0 via the CoreBoard's USB-UART bridge |
| Framework | ESP-IDF 5.x (official PIO platform) | ESP-IDF 6.1.1 (pioarduino platform) |
| cJSON | built into IDF | `espressif/cjson` from the component registry |
| Web UI build | run `gen_web.py` by hand | runs automatically before each build |

Why PARLIO instead of LCD_CAM: the S31 has both. Its i80 LCD bus is at least
8 bits wide and needs WR and DC pins too, so four of the hat's signals would
need six more real GPIOs toggling at 40 MHz. PARLIO TX carries GCLK + A/B/C on
exactly the four pins the hat wires, and it keeps the S3 engine's model: one
DMA buffer per scan cycle, a feeder task that keeps the queue full, and an
exact-cycle pause for each vsync.

Everything else carries over unchanged: frame structure, latch codes, config
payloads, the solved pixel mapping, 128 latches + vsync-only frames, GCLK
stopped at a cycle boundary with the address resting at 0, and the
core 0 / core 1 split (content + network on core 0; panel on core 1).

## Bring-up checklist (new hat)

Nothing here has been run on hardware yet. Suggested order:

1. **Without the panel**: `pintest 6` and check each buffered signal on the
   IDC pins with the logic analyzer. Then `scan start` + `status` and check the
   GCLK bursts and address stepping on IDC 11/8/7/6. Expect ~1041 pulses per
   phase at 20 MHz and the address resting at 0 between frames.
2. Wire the panel and power it. The UI starts the default animation. If the
   image is wrong, open Advanced → **Take manual control** and use
   `pat border` (one red top row, blue bottom, green left, white right).
3. If anything looks garbled, drop the clocks with **OEM-like timing**, then
   go back up. `oc status` shows the actual rates.
4. If PARLIO misbehaves, `scan backend timer` switches to the bit-bang scan
   (slow GCLK, but independent of PARLIO). `data backend bitbang` does the
   same for the data side.

## Layout

```
src/
  main.c            bring-up order (panel init runs on core 1)
  pins.h            S31 hat GPIO map
  datapath.[ch]     bit-bang data engine (commands, vsync, fallback)
  datapath_qspi.*   quad-SPI DMA frame path
  scanout.[ch]      scan backend switch + bit-bang scan
  scanout_par.*     PARLIO DMA scan engine (S31-specific)
  protocol.[ch]     frame structure: latches, vsync, config, pacing
  framebuf.[ch]     solved pixel mapping (slot, word) -> (x, y)
  canvas.[ch]       RGB drawing surface + gamma/brightness + slot packing
  effects.c         animations (incl. beat-synced), text animations, clock
  effects_art.c     3D & shapes effects (wireframes, donuts, fractals, ...)
  effects_theme.c   horses & birthday party effects (pixel-art sprites)
  render.[ch]       two-core pipeline (+ beat pulse, auto-change, sample texts)
  beatclock.[ch]    beat clock locked to Mixxx reports (no ESP-IDF deps)
  beatsync.[ch]     UDP 4210 listener, per-frame beat position, sampler state
  samples.[ch]      sample texts shown while a Mixxx sampler plays
  settings.[ch]     NVS-persisted configuration
  wifi_mgr.[ch]     station + setup AP + SNTP
  webserver.c       REST + WebSocket state push
  console.c         serial / web-terminal commands
  web/index.html    the UI
tools/gen_web.py    embeds the UI (runs automatically)
```
