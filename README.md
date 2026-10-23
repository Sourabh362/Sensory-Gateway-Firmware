# Sensor Gateway Firmware

Firmware for a sensor gateway: receives frames over UART, finds every intact reading in a noisy
stream, stores readings in NOR flash without losing them on power cuts, and offers a debug console.
The hardware (UART, 1 ms tick, NOR flash, console) is simulated on a PC; the firmware core is
portable C with no OS calls and no heap.

- **Visual flow charts:** [docs/flow.md](docs/flow.md)
- **How it works, how to test it, trade-offs at a glance:** [docs/explanation.md](docs/explanation.md)
- **Every design decision with full reasoning:** [docs/decisions.md](docs/decisions.md)

## Layout

```
include/hal.h   hardware interface: the only way core/ reaches hardware
core/           firmware: SPSC queues, CRC, parser, flash log, stages, console
sim/            simulated hardware (threads + files) and main.c
tests/          unit tests, power-cut torture test
tools/          capture generator, end-to-end test
samples/        capture files (.bin) and manifests (.json)
```

```
UART ISR ─► [byte ring] ─► parse ─► [reading queue] ─► save ─► flash log ─► NOR flash
console  ─► stats / dump <n> / erase (dump and erase go through the save stage)
```

## Build and test

Requires a C11 compiler, CMake ≥ 3.16 and Python 3 (macOS, Linux or WSL).

```sh
make          # build (strict warnings: -Wall -Wextra -Werror -pedantic)
make test     # unit tests + power-cut torture test + end-to-end runs
make clean
```

## Run

```sh
./build/gateway --capture samples/clean.bin --exit-when-done
./build/gateway --capture samples/noisy.bin --exit-when-done
./build/gateway --capture samples/burst.bin --baud 921600 --exit-when-done
./build/gateway --capture samples/noisy.bin       # interactive
```

Interactive commands: `stats`, `dump <n>` (newest first), `erase`. Ctrl+C quits.

| Option | Meaning |
|---|---|
| `--capture FILE` | raw UART byte stream to replay |
| `--baud N` | UART rate, 8N1 (default 115200; 0 = as fast as possible) |
| `--flash FILE` | flash image, kept across runs (default `flash.img`; delete it for a fresh chip) |
| `--cut-after-bytes N` | power cut after N flash bytes programmed |
| `--cut-on-erase K` | power cut halfway through erase number K |
| `--exit-when-done` | print `stats` and exit once everything is stored |

## Test captures

- `samples/*.bin` + `*.json`: Fluxton's captures and manifests; `gen_frames.py` makes more.
- `samples/generated/`: extra captures from our own `tools/make_capture.py` (adds unknown-type frames).
- `tools/e2e_test.py` replays a capture through the gateway and checks the stats against its manifest
  (all of these run in `make test`).

```sh
python3 gen_frames.py --profile noisy --seed 7 --readings 500 --p-corrupt 0.3 \
    --out /tmp/hard.bin --manifest /tmp/hard.json
python3 tools/e2e_test.py build/gateway /tmp/hard.bin /tmp/hard.json
```

## Results

Fluxton's samples (`samples/*.bin`), fresh flash each run, real baud rate:

| Capture | Baud | Bytes received / lost | Readings found / stored (manifest) | Heartbeats (manifest) | Dropped |
|---|---|---|---|---|---|
| clean | 115200 | 22,400 / 0 | 2,000 / 2,000 (2,000) | 80 (80) | 0 |
| noisy | 115200 | 36,695 / 0 | 2,714 / 2,714 (2,714) | 110 (110) | 0 |
| burst | 921600 | 223,294 / 0 | 19,784 / 19,784 (19,784) | 198 (198) | 0 |

Burst is stored with zero loss (stretch goal). It wraps the flash: the newest 8,031 readings are kept,
the oldest are discarded by design.

<details>
<summary><code>stats</code> output for each sample</summary>

```
clean.bin @ 115200          noisy.bin @ 115200          burst.bin @ 921600
uptime_ms         1944      uptime_ms         3185      uptime_ms         2423
bytes_received   22400      bytes_received   36695      bytes_received  223294
bytes_lost           0      bytes_lost           0      bytes_lost           0
readings          2000      readings          2714      readings         19784
heartbeats          80      heartbeats         110      heartbeats         198
crc_errors           0      crc_errors         389      crc_errors         289
len_errors           0      len_errors         496      len_errors         289
unknown_frames       0      unknown_frames       0      unknown_frames       0
bytes_discarded      0      bytes_discarded   6291      bytes_discarded   4680
readings_dropped     0      readings_dropped     0      readings_dropped     0
readings_in_flash 2000      readings_in_flash 2714      readings_in_flash 8031
readings_stored   2000      readings_stored   2714      readings_stored  19784
sectors_in_use       4      sectors_in_use       6      sectors_in_use      16
sectors_erased       0      sectors_erased       0      sectors_erased      23
readings_discarded   0      readings_discarded   0      readings_discarded 11753
bad_records          0      bad_records          0      bad_records          0
```

</details>

`crc_errors` and `len_errors` are higher than the injected damage counts because every false start
(a stray `0xAA` in noise) is also counted when the parser rejects it and rescans.

## Power-loss testing

- **Unit level (`test_flashlog`):** 1,500 random power cuts (1,200 mid-write, 299 mid-erase) over
  ~436k readings and many wrap-arounds. After every reboot, every reading reported as stored is
  present, in order, with no duplicates, and logging continues.
- **Manual demo:**

```sh
rm -f flash.img
./build/gateway --capture samples/noisy.bin --cut-after-bytes 5003 --exit-when-done
# *** POWER CUT *** last reading reported stored: ts=... ms
./build/gateway --capture samples/noisy.bin      # type: dump 3
```

## Assumptions

- The sensor only broadcasts; the gateway never transmits to it.
- "Stored" means the flash write has completed; only those readings are promised to survive.
- Heartbeats are counted, not stored.
- `bytes_discarded` counts bytes skipped while looking for a frame start, plus dropped false starts.
- Temperatures are shown in °C from hundredths; timestamps are the sensor's own.

## Known limitations

Full list in [docs/decisions.md §12](docs/decisions.md). The main ones:

- CRC-16 limits: noise or a torn record forms a valid CRC about 1 in 65,536 times.
- Bit flips are detected and skipped, not repaired; bad sectors are not remembered across reboots.
- Encryption and telemetry uplink are designed-in hooks, not implemented.

## Next steps

- Design document in `docs/` (block diagram, parser states, flash layout, boot steps).
- Wear statistics (erase count per sector), CI, parser fuzzing, Cortex-M cross-compile.
