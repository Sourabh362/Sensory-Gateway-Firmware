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

### UART ring buffer concurrency: single producer, single consumer, atomic indices

**Status:** Accepted

**Target:** a **single-core** microcontroller. The UART RX interrupt *preempts* the main loop, runs to completion, and returns. Only one of them runs at any instant, but the ISR can fire at **any point** in the middle of main-loop code.

**Model: one producer, one consumer (SPSC)**

```
 UART RX ISR (producer)                      main loop (consumer)
 ──────────────────────                      ────────────────────
 writes: data[head], head                    writes: tail
 reads : tail  (is it full?)                 reads : head (anything new?), data[tail]
 counts: bytes_received, bytes_lost          reads : counters for `stats`
```

**The rules that make it safe:**
1. **Every shared variable has exactly one writer.**
   - `head` is written only by the ISR.
   - `tail` is written only by the main loop.
   - `bytes_received` and `bytes_lost` are written only by the ISR.

   No variable is ever written by both sides, so there's no read-modify-write race and no lock is needed.
2. **Index reads and writes are atomic.** Each one is a single indivisible 32-bit access, so the other side never sees a half-updated value.
3. **Ordering: data first, then index.**
   - The ISR stores the byte into `data[head]`, then publishes `head + 1` (*release*).
   - The main loop reads `head` (*acquire*), then reads the data.

   So if the main loop sees the new index, the byte is guaranteed to be there already. The same applies in reverse for `tail`: the slot is only reused after the main loop has finished reading it.
4. **The ISR never waits.** If the buffer is full, it drops the byte, increments `bytes_lost`, and returns. No locks, so no deadlock and no priority inversion.

**Where atomics are used, and where they are not:**

| Variable | Atomic | Reason |
|---|---|---|
| Ring `head` | Yes | Written by the ISR, read by the main loop |
| Ring `tail` | Yes | Written by the main loop, read by the ISR |
| `bytes_received`, `bytes_lost` | Yes | Written by the ISR, read by the main loop |
| Ring data bytes | No | Protected by the index ordering (rule 3) |
| Reading queue (parser → flash) | No | Producer and consumer both run in the main loop, so nothing is concurrent |

**Why C11 atomics instead of just `volatile`:**
- On a single core, `volatile` plus a compiler barrier is enough in practice. The only risk there is the **compiler** reordering the data write after the index write.
- C11 `atomic_load_explicit` / `atomic_store_explicit` with acquire/release states the intent explicitly, and it is **also correct on multi-core** (ESP32 dual-core, and the PC simulation, where the "ISR" is a real parallel thread). On multi-core, the *hardware* can also reorder memory, and acquire/release prevents that.
- Cost on a single-core Cortex-M: plain loads and stores plus a barrier. Effectively free.
- The counters avoid atomic read-modify-write (`fetch_add`). Since only the ISR writes them, it does a plain atomic load followed by an atomic store. That stays lock-free even on Cortex-M0, which has no atomic RMW instructions.

**Rejected alternatives:**

| Alternative | Why not |
|---|---|
| Disable interrupts around main-loop buffer access (critical section) | Works on a single core, but adds interrupt latency and does not work across cores |
| Mutex / lock | An ISR must never block on a lock |
| RTOS queue (`xQueueSendFromISR`) | Fine on FreeRTOS, but the brief requires a self-written buffer, and it hides the reasoning |

### Buffer sizing: large, safe buffers for UART overflow and flash delays

**Status:** Accepted (exact sizes to be confirmed by testing)

There are two buffers, and each protects against a different slow-down:

```
UART ISR ─► [1] byte ring buffer ─► parser ─► [2] reading queue ─► flash writer
             protects UART RX                  protects against flash delays
```

**Sizing rule:** a buffer must hold everything that arrives during the **longest time its reader can be blocked**, plus a safety margin.

`size ≥ incoming rate × worst-case block time × margin (≥2×)`

#### [1] Byte ring buffer: prevents UART overflow

- **Writer:** UART RX interrupt. **Reader:** main loop.
- The main loop is blocked for at most one flash operation: a sector erase, **20 ms**.

| Baud | Bytes/s (8N1 = 10 bits/byte) | Bytes arriving in 20 ms | Chosen size | Headroom |
|---|---|---|---|---|
| 115200 | 11,520 | ~230 B | **4 KB** | ~350 ms (≈17×) |
| 921600 | 92,160 | ~1,840 B | **4 KB** | ~44 ms (≈2×) |

- If it is ever full, the ISR **drops the byte and counts it** (`bytes lost`). It never waits or blocks.
- The size is a power of two, so the index wrap is a cheap bit-mask.

#### [2] Reading queue: absorbs flash delays

- **Writer:** parser (good readings). **Reader:** flash writer.
- Flash work blocks the writer: 20 ms per sector erase, or 16 × 20 ms = 320 ms for the `erase` console command.

| Situation | Readings arriving (≈1,000/s at 115200) | Chosen size |
|---|---|---|
| One sector erase (20 ms) | ~20 | |
| `erase` command (320 ms) | ~320 | **512 readings** (≈1.6× margin) |

- If it is full, the reading is **dropped and counted** (`readings dropped`).
- When a backlog builds, the flash writer batches several records into one write (up to 256 B) to catch up faster.

#### Important limit: buffers absorb *temporary* stalls, not *sustained* overload

- At 115200 the flash is busy ~28% of the time, so the buffers drain between stalls. No loss.
- At 921600 (`burst.bin`) the flash needs ~2.2 s of work per 1 s of data. No buffer size can fix a rate mismatch; any buffer just fills more slowly. Loss there is expected, counted and explained. The real fix is making flash work cheaper (batching, smaller records), not a bigger buffer.

#### Constraints on the buffers
- **Statically allocated** at compile time. No heap after start-up (firmware rule).
- RAM cost: ~4 KB (bytes) + ~4 KB (512 readings × 8 B) ≈ **8 KB**, fine for a typical MCU (ESP32 has 320+ KB).

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