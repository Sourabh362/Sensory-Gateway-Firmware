# Sensory-Gateway-Firmware

## Design Decisions & Optimizations

### Store every reading (store-on-change / interval considered)

**Status:** Store every reading (as the brief requires). Store-on-change is a documented optimization that can be added later.

**Idea considered:** do not write every reading to flash. Instead, store a reading only when:
- the value changes from the last stored reading, or
- a regular interval passes (for example at heartbeat times).

This is *report-by-exception* (a deadband). Real gateways use it widely because it:
- saves flash space, so more history fits in 64 KB,
- reduces erase cycles, so flash lasts longer,
- reduces flash busy time, which helps at high data rates (burst).

**Why it is not used here:**
- The brief says **"Store every reading"**. For `clean.bin` and `noisy.bin` the pass criterion is **"every intact reading stored"**, checked against the manifest counts.
- Every reading carries its own sensor **timestamp**. Two readings with the same temperature are still two different data points, so skipping one loses information.

**Decision:** every valid reading frame is stored in flash. Heartbeat frames are counted, not stored.

**How it could be implemented later:** as a configurable *storage policy* filter between the reading queue and the flash writer:

```
parser ─► reading queue ─► [storage policy] ─► flash writer
                            ├─ STORE_ALL      (current)
                            ├─ STORE_ON_CHANGE (store if |Δtemp| > deadband)
                            └─ STORE_INTERVAL  (store every N ms / on heartbeat)
```

Only the filter changes. The parser, flash log and power-loss handling stay the same.

**Lossless alternatives also considered:**

| Method | Saves space | Why not used |
|---|---|---|
| Delta encoding (store changes from the previous reading) | Yes, ~4–6 B per record | A record depends on the previous one, so one torn record breaks the chain and power-loss recovery gets much harder |
| Run-length ("same value, N more readings") | Yes, if values are steady | Variable-size records make boot recovery complex |
| Fixed standalone records | Moderate | **Chosen.** Each record can be verified on its own, which is simplest for power-loss safety |

### Noise handling: detect, drop, resync (no repair)

**Status:** Accepted

**Data path:**

```
UART ISR ─► ring buffer (raw bytes, no checks) ─► parser (SOF / LEN / CRC checks) ─► valid frames
```

The ring buffer only moves raw bytes from the interrupt to the main loop. The parser reads them one at a time and decides which bytes form a valid frame. That is where noise is overcome.

**Checks available in the frame format:**

| Check | Purpose |
|---|---|
| SOF = `0xAA` | Marks where a frame *might* start. Weak on its own, because `0xAA` also appears in noise and payloads |
| LEN ≤ 64 | Cheap early rejection of false starts |
| CRC-16/CCITT-FALSE | Final judge: any corruption in LEN/TYPE/PAYLOAD fails it |
| TYPE + LEN match (`0x01`→6, `0x02`→0) | A frame with a valid CRC but the wrong shape is counted as *unknown* |

**Recovery: rescan after a false start.** Every SOF is treated as a guess. If the guess fails (bad LEN or bad CRC), only the first byte (the false `0xAA`) is dropped, and the parser rescans from the very next byte using the bytes it had buffered (max 69 = one full frame).

```
37 AA 05 AA 06 01 29 09 E8 03 00 00 7B 84
── noise ── └──────── good reading ───────┘

AA 05 ...      LEN=5, CRC fails → drop first AA, rescan from "05 AA 06 ..."
AA 06 01 ...   CRC OK → reading found ✓
```

Rejected alternative: on a bad frame, skip the whole SOF+LEN packet. A good frame that starts *inside* those bytes would be lost.

| Noise type | Result |
|---|---|
| Random junk | Discarded until the next `0xAA` |
| Junk containing `0xAA` | False start fails its CRC; rescan finds the real frame |
| Corrupted frame (bit errors) | CRC fails, counted as a CRC error, rescan |
| Truncated frame | Its "frame" swallows the next frame's start, CRC fails, rescan finds the next frame |
| LEN > 64 | Length error, immediate rescan |

**What this achieves, and what it doesn't:**
- ✅ **Every intact frame is found**, whether before, after or inside noise. A byte is only thrown away once it is proven not to start a valid frame.
- ✅ **Every damaged frame is counted** (CRC errors, length errors, unknown frames, bytes discarded).
- ❌ **Damaged frames are not repaired.** Their original data is lost. CRC-16 could in theory correct a single flipped bit, but that only works for one-bit errors and raises the risk of "repairing" garbage into a fake reading. For a data logger, a wrong stored value is worse than a missing one.

**Known limit:** noise that happens to form a frame with a correct CRC (≈1 in 65,536 per false start) would be accepted. This is inherent to CRC-16.

**On real hardware, extra sync hints exist that the simulation cannot provide:**
- UART idle-line / RX timeout detection (gaps between frames)
- UART framing and parity error flags