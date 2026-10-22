# Design Decisions & Trade-offs

How the gateway was designed: what was considered, what was chosen, and why.
Struck-through lines are decisions that were later superseded; the note says what replaced them.

## 1. Store every reading (store-on-change considered)

- **Store every valid reading in flash.** The brief requires "store every reading", and clean/noisy are checked against the manifest counts.
- **Heartbeats are counted, not stored.** They carry no data, only "link alive".
- **Store-on-change / interval (deadband) rejected.** Every reading has a unique timestamp, so skipping "same value" readings loses data.
- **Can be added later as a storage-policy filter** (queue → filter → flash writer). Only the filter changes; the parser and flash log stay the same.
- **Delta encoding rejected.** Each record depends on the previous one, so one torn record breaks the chain.
- **Run-length encoding rejected.** Variable-size records make power-loss recovery complex.
- **Fixed standalone records chosen.** Each record can be verified on its own.

## 2. Flash record format: 8 bytes = payload + the frame's CRC

```
[temp 2B][timestamp 4B][CRC 2B]      e.g. frame AA 06 01 |29 09 E8 03 00 00| 7B 84
                                                    stored → 29 09 E8 03 00 00  7B 84
```

- **Only the payload + the frame CRC are stored.** That is all the data plus the minimum needed to detect corruption.
- **SOF/LEN/TYPE are not stored.** They are always `AA 06 01` for a reading, so they are re-added when validating.
- **The frame's CRC is reused.** It was already computed and verified by the parser, so there is no extra work on the write path.
- **Written in one 8-byte write.** It is just as safe as two writes (a missing CRC reads `FFFF` and fails) and twice as fast.
- **"Stored" is reported only after the write completes.** A power cut before that point loses nothing that was promised.
- **Validation is `crc16(06 01 + payload) == stored CRC`.** The stored CRC is the fingerprint taken while the data was good.
- **The CRC must be stored, not just recomputed.** Recomputing from the data alone compares it with itself and always "matches".
- **All-`FF` slot = blank.** `crc16(06 01 FF×6) = 0x9E4B ≠ 0xFFFF`, so a blank slot can never pass as valid.
- **Torn or corrupted slot (CRC mismatch) is skipped and never reused.** NOR cannot be rewritten without an erase.
- **No data-only 6 B record.** A torn record would look like a valid reading (e.g. `t = 4,294,967,272 ms`).
- **No 16 B record (seq + commit byte).** It needs 2 writes per reading and halves capacity; order already comes from sector seq + slot position.
- **No parity bit.** It misses ~50% of torn writes.
- **No 0-bit count (Berger code).** It catches all torn writes but misses two-way bit flips from wear/decay.
- **The CRC is not a speed issue.** It takes ~0.5 µs vs 100 µs per flash write, and reusing the frame CRC removes even that.
- **Capacity: 511 records per sector, ≈ 8,176 readings in total.** 8-byte records never straddle a 256 B page.
- **Known limit: a torn record passes by chance ~1 in 65,536.** It can only happen at a power cut, at most once per cut.
- **Known limit: the format is tied to the sensor frame format.** Acceptable because the protocol is fixed by the brief.

## 2a. Storage encoding: raw binary, optional encryption hook (open)

- **Records are stored as raw binary, not hex or ASCII.** This is the most compact form: hex/ASCII text would double or triple the size (8 B → 16+ B).
- **Little-endian, packed, fixed 8 B.** It matches the wire format, so it is copied with no conversion.
- **Encryption is an open block: designed in, disabled by default.** It is not required by the brief, but the slot exists so it can be enabled later.

```
reading ─► [encrypt? (off by default)] ─► 8 B record ─► flash
flash ─► 8 B record ─► [decrypt? (off by default)] ─► CRC check ─► reading
```

**Rules for when encryption is enabled (to be implemented later):**
- **The record stays 8 B (size-preserving cipher, e.g. AES-CTR).** The flash layout, capacity and boot scan stay unchanged.
- **Encrypt the payload + CRC together.** The CRC is not left in clear, so no information leaks.
- **Blank check (all `FF`) happens before decrypting.** A blank slot is detected without the key.
- **Validate as decrypt → CRC check.** Torn/corrupted records still fail exactly as before.
- **Nonce = sector seq + slot index.** It is unique per write, because a slot is written once per erase and the sector seq always increases.
- **The key lives outside flash** (secure element / eFuse on real hardware). A key stored next to the data protects nothing.
- **Known limit: encryption gives confidentiality, not tamper-proofing.** Detecting deliberate modification needs a MAC, which grows the record (trade-off left open).

## 2b. Flash layout: portable, geometry from the HAL

```
Flash region [base, base+size)  →  N sectors, used as a ring: 0 → 1 → … → N-1 → 0
Sector:  [header 8B][rec 0][rec 1] … [rec M-1]     unused slots stay FF
Record:  [temp 2B][timestamp 4B][CRC 2B]
```

- **Layout = how the whole flash region is organized; record format = how one reading is stored.** Boot recovery reads this structure to rebuild state after a power cut.
- **Geometry comes from the HAL (`hal_flash_get_info`: base, size, sector size, page size).** No hard-coded 64 KB or 16 sectors, so a different chip or partition needs no layout code change.
- **The log uses a region `[base, base+size)`, not the whole chip.** It maps directly to an ESP-IDF data partition and leaves room for firmware/config.
- **Sector count, records per sector and capacity are derived at boot.** `N = size / sector_size`, `M = (sector_size - 8) / 8`.
- **No RAM arrays sized by sector count.** Per-sector info is recomputed by scanning (reads are free), so bigger flash costs no RAM.
- **Geometry is checked at boot, and the log refuses to start with a clear error if invalid.** It needs ≥ 2 sectors, sector size a multiple of 8, page size ≥ 8, and size a multiple of sector size.
- **The record (8 B) and header format are fixed.** They are our on-flash data format, not hardware properties.
- **Sectors are used in ring order, records left to right, never overwritten in place.** NOR can only be rewritten after an erase.
- **Write order is always erase → header → records.** Any power cut leaves a state boot can recognize.
- **Sector header contents:** magic, sector seq, CRC (see §8).

| Flash | Sectors | Readings stored |
|---|---|---|
| 64 KB sim (brief) | 16 | ~8,176 |
| 1 MB partition, 4 KB sectors | 256 | ~130,800 |
| 16 KB small MCU region | 4 | ~2,044 |

## 3. UART ring buffer: single producer, single consumer, atomic indices

- **Target: a single-core MCU.** The ISR preempts the main loop at any point, so shared data must be safe at every instruction.
- **One producer (UART ISR), one consumer (main loop).** It is the simplest model that needs no locks.
- **`head` is written only by the ISR, `tail` only by the main loop.** One writer per variable means no write-write race.
- **`bytes_received` / `bytes_lost` are written only by the ISR.** Plain atomic load and store, no read-modify-write, so it is lock-free even on Cortex-M0.
- **Atomic `head`/`tail` access.** The other side never sees a half-updated index.
- **Data is written first, then the index (release/acquire).** If the reader sees the new index, the byte is guaranteed to be there.
- **The data bytes themselves are not atomic.** They are protected by the index ordering.
- **The ISR never waits.** When the buffer is full it drops and counts the byte, so there is no deadlock and no priority inversion.
- **The UART register is already `volatile`.** The vendor header handles hardware → CPU reads.
- **The ring buffer needs atomicity, not `volatile`.** `volatile` belongs to a memory location; it does not carry over to copied data.
- **C11 atomics over hand-made `volatile` + barriers.** The ordering is explicit, also correct on multi-core (ESP32, the PC simulation), and free on Cortex-M.
- ~~**The reading queue has no atomics.** Its producer and consumer both run in the main loop.~~ **Superseded by §6:** stages may run on different cores, so every queue is an atomic SPSC ring.
- **No disabling interrupts.** It adds latency and does not work across cores.
- **No mutex.** An ISR must never block.
- **No RTOS queue.** The brief requires a self-written buffer.

```c
void uart_rx_isr(void) {
    uint8_t b = UART->DR;   // volatile register read (vendor header)
    ringbuf_push(&rx, b);   // plain RAM: safe via atomic head/tail
}
```

## 4. Buffer sizing: large, safe buffers

- **Sizing rule: size ≥ incoming rate × longest reader block × margin (≥2×).** The buffer must outlast the worst stall.
- **Byte ring buffer: 4 KB.** It covers a 20 ms flash erase: ~230 B at 115200 (17× headroom), ~1,840 B at 921600 (2×).
- **Power-of-two size.** The index wrap is a cheap bit-mask.
- **Reading queue: 512 readings.** It covers the 320 ms `erase` command (~320 readings at 115200).
- **On a full queue the reading is dropped and counted.** Loss is always visible in `stats`.
- **Backlog → batch writes (up to 256 B).** This catches up faster after a stall.
- **Buffers absorb temporary stalls, not sustained overload.** A rate mismatch can only be fixed by making the consumer faster.
- ~~At 921600 flash needs ~2.2 s of work per 1 s of data, so loss is expected.~~ **Superseded by §7:** with 8 B records and batched writes, flash needs only ~0.34 s per 1 s, so burst fits.
- **Static allocation only, ~8 KB of RAM.** No heap after start-up (firmware rule).

## 5. Noise handling: detect, drop, resync (no repair)

- **The ring buffer carries raw bytes; the parser does all the checks.** The ISR stays tiny.
- **SOF `0xAA` = a possible frame start.** It is only a guess, because `0xAA` also appears in noise and payloads.
- **LEN > 64 → length error.** A cheap early rejection of false starts.
- **CRC-16 is the final judge.** Any corruption in LEN/TYPE/PAYLOAD fails it.
- **Valid CRC but wrong TYPE/LEN → unknown frame, counted.** The brief requires counting it.
- **On a failure, drop only the first byte and rescan the buffered bytes.** A good frame may start inside the damaged one.
- **No skipping the whole bad packet.** It would swallow a good frame hidden inside.
- **Rescan buffer = 69 bytes (one max frame), static.** No heap needed.
- **Every intact frame is found.** A byte is discarded only once it is proven not to start a valid frame.
- **Every damaged frame is counted.** `stats` shows CRC / length / unknown / discarded counts.
- **Damaged frames are not repaired.** A wrong stored value is worse than a missing one.
- **Known limit: noise forming a valid CRC is accepted (~1 in 65,536).** This is inherent to CRC-16.
- **Real-hardware extras: UART idle-line timeout, framing/parity flags.** The capture files carry no timing or error flags.

```
37 AA 05 AA 06 01 29 09 E8 03 00 00 7B 84
AA 05 ... CRC fails → drop first AA, rescan → AA 06 01 ... CRC OK ✓
```

## 6. Execution model: independent stages connected by queues (core-agnostic)

Two methods were considered for getting parsed readings into flash.

**Method 1: decoupled stages with queues (CHOSEN)**

```
ISR ─► [byte ring] ─► PARSE stage ─► [reading queue] ─► SAVE stage ─► flash
```

**Method 2: parse and save in one blocking flow (REJECTED)**

```
[byte ring] ─► PARSE ─► SAVE (wait for flash) ─► next byte ...
```

**Why method 1:**
- **It keeps the UART ring safe from overflow.** Parsing never stalls on flash, so the byte ring keeps draining even during a 20 ms erase or the 320 ms `erase` command.
- **Each stage is an independent task with clear input and output.** It reads its input queue and populates its output queue; the next stage takes it from there.
- **Queues are preallocated at start-up.** They are fixed-size and static, with no heap and no runtime allocation failures.
- **Queues are sized so nothing is dropped at normal rates.** Clean/noisy at 115200 never fill them.
- **Under sustained overload a full queue drops and counts, never blocks.** At 921600 no finite queue can keep up; whole readings are dropped and counted, and the UART ring stays safe.
- **Overload loses whole readings, not raw bytes.** With method 2 the byte ring would overflow, turning good frames into CRC errors.
- **It is core-agnostic.** The same stage code runs in a single-core super loop, as RTOS tasks, or on separate cores in parallel.
- **It is the same pattern as RTOS queues** (`xQueueSend` → `xQueueReceive`), but self-written, so it also works bare-metal.

**Why not method 2:**
- **Parsing stops during every flash operation.** Only the UART byte ring absorbs the delay.
- **The `erase` command (320 ms) nearly fills the 4 KB ring at 115200** (~3.7 KB arrives).
- **It cannot use a second core.** One core always waits on flash.
- **Kept as the documented simple alternative:** fine for a slow, single-core logger.

**Stages as non-blocking step functions:**

```c
bool parse_step(void);    // byte ring    → reading queue
bool save_step(void);     // reading queue → flash (one write or one erase unit)
bool console_step(void);  // command mailbox → requests to the save stage / output
```

- **Each step does a bounded amount of work and returns.** No stage can starve another.
- **The return value says whether work was done.** The caller can sleep when everything is idle.

**Same code, different deployments (only the glue changes):**

```c
/* single core: super loop */
while (1) {
    bool busy = parse_step() | save_step() | console_step();
    if (!busy) wait_for_interrupt();
}

/* two cores / RTOS tasks */
core0: while (1) { parse_step(); console_step(); }
core1: while (1) { save_step(); }
```

**Rules that make it safe on any number of cores:**
- **Every queue is SPSC with atomic head/tail** (byte ring, reading queue, command mailbox). One generic lock-free ring is used for all of them.
- **Each resource has exactly one owner.** Only the save stage touches flash; two writers would corrupt it.
- **Other stages send requests to the owner.** Console `erase`/`dump` are posted to the save stage's mailbox, not done directly.
- **Each counter has one writer (its owning stage) and is atomic.** `stats` can be read from any core without locks.
- **No other shared mutable state.** Nothing else can race.
- **No locks anywhere.** No deadlock, and the ISR never blocks.

**Cost:** on a single core the atomics compile to plain loads and stores, so it is effectively free. On multi-core, parsing and saving run in true parallel.

## 7. Flash throughput, overload policy, noise limits

**Flash keeps up through optimized handling:**
- **Design target: zero loss at all 3 sample rates, including burst at 921600.** Flash handling is optimized instead of accepting loss.
- **Batched writes: when readings are waiting, write up to 32 records (256 B, one page) in one write.** This gives ~30× fewer write operations under load.
- **One reading waiting → a single 8 B write.** No added latency at low rates.
- **8 B records → 511 per sector.** Half the erases of a 16 B design.
- **Just-in-time erase.** No erases at all during the first lap on a fresh flash.

| Burst budget (~7,900 readings/s) | Flash time per second |
|---|---|
| Writes: ~247 batched writes × 100 µs | ~25 ms |
| Erases: ~15.5 sectors × 20 ms | ~310 ms |
| **Total** | **~335 ms → ~34% busy, keeps up** |

- **Worst single stall = one 20 ms erase.** ~1,840 B fits in the 4 KB UART ring, and ~160 readings fit in the 512-reading queue.

**Overload safety net (should never trigger):**
- **If the reading queue is ever full, drop the whole reading and count it (`readings dropped`).** The loss is exact and visible.
- **Never drop at the UART ring.** Lost bytes break frames into CRC errors and can form fake frames.
- **The parse stage never waits on a full queue (no backpressure).** Waiting would push the loss back into the UART ring.
- **Every byte is received and every frame is CRC-checked even under overload.** Parser stats stay exact.
- **Expected `readings dropped` = 0 in every run.** A non-zero value means the flash limit was reached.

**Noise beyond a limit is a hardware issue:**
- **The firmware's job is to survive noise and report it.** Every intact frame is found, and every bad frame is counted.
- **A persistently noisy line needs hardware inspection** (cabling, grounding, baud mismatch, EMI). Firmware cannot fix a bad link.
- **Optional: a link-quality figure in `stats`.** `bad frames / total frames`, with a "link degraded, inspect hardware" warning above a threshold (e.g. 5%).

## 8. Sector header and boot recovery

**Sector header (8 B):**

```
[magic 2B][sector seq 4B][CRC 2B]
```

- **magic:** marks the sector as belonging to our log, as opposed to random garbage.
- **sector seq:** increases every time a sector is started, so the highest one is the newest after any number of wraps.
- **CRC:** detects a header torn by a power cut.
- **Written right after the erase, before any record.** A sector without a valid header never holds promised data.

**Recovery means two things:**
- **State recovery is done now (required).** After a reboot, find the newest data and where to continue writing; RAM is empty, so the state is rebuilt from flash.
- **Data repair is open, not done now.** Bit flips and torn records are detected by CRC, skipped and counted, but not repaired.

**Boot steps:**

| Sector looks like | Meaning | Action |
|---|---|---|
| All `FF` | Free | Leave it |
| Valid header | In use | Scan its records |
| Bad header, not blank | Power cut during erase or header write | Erase it and count it; it held nothing promised |

- **The newest sector is the one with the highest valid sector seq.** That is where writing continues.
- **Valid record (CRC OK) → counted** in `readings in flash`.
- **CRC bad (torn write or bit flip) → skipped, counted as a bad record, the slot is never reused.**
- **Writing resumes at the first blank slot after the last used one.** Torn slots are skipped forever.
- **Reads are free, so the boot scan takes only milliseconds.** No stored pointer or counter is needed.

**Erase timing:**
- **Why erase: NOR only flips bits 1→0, so a used slot can't be rewritten.** An erase resets a whole sector to `FF` (20 ms).
- **Just-in-time: erase only when the ring wraps into a sector holding old data.** Simple, full capacity.
- **Erasing drops the oldest sector's readings.** That is the brief's "when flash is full, discard the oldest".
- **Pre-erasing one sector ahead was rejected for now.** It costs one sector of capacity, and batching already keeps up.

## 9. Flash error handling

- **A write fails on a slot → don't retry it (it is no longer blank); move to the next slot and count it.** NOR can't rewrite without an erase.
- **Repeated failures in one sector → treat it as bad and move to the next sector (erase + header), counting it.** Avoids getting stuck on a failing area.
- **An erase fails → count it and skip to the next sector.**
- **Flash stays busy past a timeout → count it, give up on that operation, keep running.** The firmware never hangs on flash.
- **Bad sectors are not remembered across reboots (open).** A truly bad sector fails again and is skipped again; a persistent bad-sector table is a later option.

## 10. Console `stats`

| Group | Counters |
|---|---|
| UART | bytes received, bytes lost (ring overflow) |
| Parser | good readings, good heartbeats, CRC errors, length errors, unknown frames, junk bytes discarded |
| Queue | readings dropped (queue full) |
| Flash | readings in flash, sectors in use, sectors erased, readings discarded (oldest dropped), bad records (CRC fail), write errors, sectors skipped |
| System | uptime |

- **`dump <n>` shows the newest n readings first.** It is posted to the save stage, which owns the flash.
- **`erase` is posted to the save stage and done one sector per step.** The UART keeps draining.
- **Commands do bounded work per step.** A big `dump` never starves the UART ring.

## Open items
- Bit-flip repair (currently detect + skip + count)
- Persistent bad-sector table
- Encryption block (hook only, off by default)
- Link-quality warning threshold
- Wear-levelling stats (erase count per sector, a stretch goal)
- Telemetry output (see §11)

## 11. Telemetry output (open block): for now readings are only saved

- **Current behaviour: readings are stored in flash only.** The brief has no uplink; the only output is the debug console (`stats`, `dump`).
- **Possible outputs later:** post to a server, echo on a second UART, or send over wireless (BLE, Wi-Fi/MQTT, LoRa, cellular).
- **Store-and-forward: telemetry reads from the flash log, not from the parser.** Readings survive link outages and power cuts; nothing is lost while the radio is down.
- **Flash stays the single source of truth.** "Stored" and "sent" are separate states; a reading is never dropped just because the link is slow.
- **A telemetry stage would be one more independent stage (§6).** It has its own input/output, so the same code still runs on one core or several.
- **It must not touch the flash directly; it asks the save stage (the flash owner).** This keeps the one-owner rule, the same as `dump`.
- **Needs a "sent up to" cursor (sector seq + slot).** Kept in RAM; persisting it occasionally avoids resending everything after a reboot (trade-off: flash wear vs duplicates).
- **Receivers must tolerate duplicates after a reboot (at-least-once delivery).** The timestamp works as a de-duplication key.
- **Optional live echo: a second, droppable queue from the parse stage.** Low latency for dashboards; losses there are harmless because flash still has everything.
- **Not implemented now:** no uplink in the brief; the slot is designed in so it can be added without changing the parser or the flash log.

```
parse ─► [reading queue] ─► save ─► flash ─► (later) telemetry stage ─► uplink
                                     ▲                │
                                     └── read via save stage (flash owner)
```

## 12. Known gaps and limitations

- ~~**Tested with our own capture generator, not Fluxton's real samples yet.**~~ **Resolved:** Fluxton's `samples/` all pass end to end with exact manifest counts and zero loss; our generator is kept for extra cases (unknown-type frames).
- **The sim UART delivers bytes in ~100 µs micro-bursts.** Like a UART FIFO; the average rate is exact (clean.bin takes 1.944 s at 115200), but per-byte spacing isn't.
- **A power cut tears writes at byte granularity.** Real NOR can leave a byte half-programmed (some bits); the CRC still catches that, but the simulator doesn't model it.
- **Noise that happens to form a valid CRC is accepted (~1 in 65,536 per false start).** Inherent to CRC-16.
- **A torn record can pass its CRC by chance (~1 in 65,536).** Only possible at a power cut, at most once per cut.
- **Bit flips are detected and skipped, not repaired.** No ECC in the record format.
- **Bad sectors are not remembered across reboots.** A failing sector is skipped again after each boot.
- **Fully written but unreported readings may survive a cut.** Records of an interrupted batch can be complete in flash; that is allowed (they were never promised), but they appear in `dump` after reboot.
- **`stats` (console stage) and `dump` (save stage) both print.** If those stages ran on different cores, their lines could interleave; a single output owner would fix it.
- **Flash wear is spread by ring rotation, but erase counts are not tracked.** Wear stats are a stretch goal.
- **Encryption is a hook only (off).** No key handling or cipher yet.
- **Telemetry uplink is not implemented (§11).** Readings are saved only.
- **Sector seq is 32-bit and never wraps in practice** (~4 billion sector changes); wrap handling is not implemented.
- **The simulator's flash file is reinitialized if its size is wrong.** A wrong-sized image is treated as a fresh chip.
