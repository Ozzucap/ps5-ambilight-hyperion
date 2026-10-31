# PS5 Ambilight for Hyperion.ng
![PS5 ambilight with Hyperion](ps5-ambilight-simple.png)

Ambilight for a jailbroken PS5 **without a capture card**: a small bridge takes the
console's picture, reduces it to 64×36 pixels and feeds it to
[Hyperion.ng](https://github.com/hyperion-project/hyperion.ng), which drives your LEDs.
Inspired by [PS4-Ambilight](https://github.com/mitchshimy/PS4-Ambilight).

Tested on: PS5 Phat (CFI-1200), firmware 13.00 · Hyperion.ng in Docker on a NAS ·
WLED strip. Games only; media apps are untested. Everything here is provided as is.

## What is in this repo

| Part | What it does |
|---|---|
| `src/ps5_bridge.c` | The bridge. Receives the NV12 stream, downsamples it, converts colours (including HDR→SDR) and sends it to Hyperion. Runs on any Linux machine on your LAN. |
| `src/hyperion_fb.[ch]` | Dependency-free client for Hyperion's Flatbuffers server. |
| `src/test_sender.c` | PS5 payload that sends a moving rainbow to Hyperion. Checks the console → Hyperion → LEDs path without any capture. |
| `tools/whatapp` | PS5 payload that logs which apps are running (used to detect the home screen). GPLv3. |
| `docker-compose.yml` | Example stack for Portainer / Docker. |

### What is *not* in this repo

The payload that reads the picture on the console. I used
[PSCapture](https://github.com/fstat13/pscapture) by fstat13 for that. Its licence
allows private modification but forbids publishing modified versions, so nothing derived
from it is included here. Get it from its author, and check which firmware it supports:
on my fw 13.00 the official payload did not start. The bridge only needs a stream in the
format described below.

## How it works

```
PS5 payload ──TCP 5599 (NV12 frames)──▶ bridge ──TCP 19400 (Flatbuffers)──▶ Hyperion ──▶ LEDs
```

## Quick start

**1. Check Hyperion.** Its Flatbuffers server must be reachable (default TCP port 19400).

**2. Test the LEDs without the PS5 picture.**

```sh
make led-test PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
```

Copy `config.ini.example` to `/data/ps5-ambilight/config.ini` on the console (set `host=`
to your Hyperion machine), then send `build/ps5-ambilight-test.elf` to the console's
ELF loader. A rainbow should run around your strip for 30 s.

**3. Start the capture payload** on the console (see above).

**4. Run the bridge.**

```sh
make bridge
./build/ps5-bridge --ps5 <console-ip> --hyperion <hyperion-ip>
```

or with Docker/Portainer: copy `build/ps5-bridge` to the folder named in
`docker-compose.yml`, set the two IPs and deploy the stack.

**5. Look at `snapshot.png`** (`--snapshot` / `SNAPSHOT`). It shows what the bridge
receives; it is the quickest way to spot wrong colours or a grey strip.

## Options

Each option is a command-line flag or an environment variable.

| Flag | Env | Default | Meaning |
|---|---|---|---|
| `--ps5` | `PS5_HOST` | – | console address (required) |
| `--ps5-port` | `PS5_PORT` | 5599 | first stream port (tries up to +30 if refused) |
| `--hyperion` | `HYPERION_HOST` | – | Hyperion address (required) |
| `--hyperion-port` | `HYPERION_PORT` | 19400 | Flatbuffers port |
| `--priority` | `HYPERION_PRIORITY` | 150 | 100–199, lower wins |
| `--fps` | `FPS` | 30 | max frames/s sent to Hyperion (1–60) |
| `--crop-bottom` | `CROP_BOTTOM` | 32 | ignore the last N rows (even number) |
| `--hdr` | `HDR=1` | off | input is HDR10 (PQ, BT.2020): convert to SDR |
| `--paper-white` | `HDR_PAPER_WHITE` | 203 | HDR level (nits) that becomes SDR white; lower = brighter LEDs |
| `--hdr-peak` | `HDR_PEAK` | 1000 | HDR level (nits) that clips to full white |
| `--full-range` | – | off | input is full-range YUV |
| `--swap-uv` | – | off | use if reds and blues are swapped |
| `--snapshot` | `SNAPSHOT` | – | write a 480×270 PNG every 5 s |

The log prints `stream X fps in, Y fps to Hyperion` every 10 s, plus `gap:` / `slow:`
lines when the console stops sending or Hyperion is slow to accept a frame.

## Lessons learned

- **Dull, washed-out LEDs usually mean HDR.** A PQ signal read as SDR peaks at roughly
  two thirds of the brightness. Use `--hdr`. Forcing it on makes SDR content look dim.
- **Bottom LEDs stay lit in black scenes.** The last rows of the captured frame are written
  late and show as a grey strip; `--crop-bottom` removes them.
- **Random blackouts / frozen live view in Hyperion.** Two sources with the same priority
  take the same slot. If an Android TV grabber runs at 150, give the bridge another
  priority (e.g. 140).
- **Don't wait for Hyperion's replies.** The server answers every image. Blocking on the
  rest of a reply cost about 40 ms per frame in my tests; the client drains replies without
  blocking.
- **Short network drops.** The bridge keeps the last picture for 2 s before it releases the
  LEDs, and reconnects to Hyperion within about 300 ms.
- **Home screen.** The capture buffer is not refreshed like in a game, so a payload that
  just keeps reading shows stale colours or flicker. Games and store apps have a process
  whose title id type is `PPSA`; on the home screen only system (`NPXS`) processes exist.
  `tools/whatapp` logs this so you can check it on your console:

  ```sh
  make whatapp PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
  ```

  Run it, switch between home and a game, then read `/data/ps5-ambilight/apps.log`.

## Stream format expected by the bridge

TCP, one connection. Per frame: a 12-byte little-endian header (magic, frame counter,
payload length) followed by NV12 data (Y plane, then interleaved UV at half height).

- magic `AVS2`: 1920×1080, length 3110400
- magic `AVS3`: 480×270, length 194400 (optional pre-reduced variant)

## Credits

- [PS4-Ambilight](https://github.com/mitchshimy/PS4-Ambilight) – the idea
- [PSCapture](https://github.com/fstat13/pscapture) by fstat13 – console-side capture and
  reverse-engineering notes (`docs/REVERSE_ENGINEERING.md`); not included here
- [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) by John Törnblom – toolchain;
  `tools/whatapp` is based on its `ps` sample
- [Hyperion.ng](https://github.com/hyperion-project/hyperion.ng) – Flatbuffers schema
  `libsrc/flatbufserver/hyperion_request.fbs`
- HDR conversion: SMPTE ST 2084 (PQ), ITU-R BT.2020 / BT.709

Developed with the help of an AI assistant (Claude).

## Licence

MIT (see `LICENSE`), except `tools/whatapp`, which is GPLv3 (see `tools/whatapp/COPYING`)
because it derives from the SDK sample.

## Disclaimer

Payloads run on a jailbroken console and can crash it. Use at your own risk.
