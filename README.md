# Sensory-Gateway-Firmware

## Design Decisions & Optimizations

### 1. Store every reading (store-on-change considered)

- **Store every valid reading in flash.** The brief requires "store every reading", and clean/noisy are checked against the manifest counts.
- **Heartbeats are counted, not stored.** They carry no data, only "link alive".
- **Store-on-change / interval (deadband) rejected.** Every reading has a unique timestamp, so skipping "same value" readings loses data.
- **Can be added later as a storage-policy filter** (queue → filter → flash writer). Only the filter changes; the parser and flash log stay the same.
- **Delta encoding rejected.** Each record depends on the previous one, so one torn record breaks the chain.
- **Run-length encoding rejected.** Variable-size records make power-loss recovery complex.
- **Fixed standalone records chosen.** Each record can be verified on its own.

### 2. Flash record format: 8 bytes = payload + the frame's CRC

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

### 2a. Storage encoding: raw binary, optional encryption hook (open)

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

### 3. UART ring buffer: single producer, single consumer, atomic indices

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
- **The reading queue has no atomics.** Its producer and consumer both run in the main loop.
- **No disabling interrupts.** It adds latency and does not work across cores.
- **No mutex.** An ISR must never block.
- **No RTOS queue.** The brief requires a self-written buffer.

```c
void uart_rx_isr(void) {
    uint8_t b = UART->DR;   // volatile register read (vendor header)
    ringbuf_push(&rx, b);   // plain RAM: safe via atomic head/tail
}
```

### 4. Buffer sizing: large, safe buffers

- **Sizing rule: size ≥ incoming rate × longest reader block × margin (≥2×).** The buffer must outlast the worst stall.
- **Byte ring buffer: 4 KB.** It covers a 20 ms flash erase: ~230 B at 115200 (17× headroom), ~1,840 B at 921600 (2×).
- **Power-of-two size.** The index wrap is a cheap bit-mask.
- **Reading queue: 512 readings.** It covers the 320 ms `erase` command (~320 readings at 115200).
- **On a full queue the reading is dropped and counted.** Loss is always visible in `stats`.
- **Backlog → batch writes (up to 256 B).** This catches up faster after a stall.
- **Buffers absorb temporary stalls, not sustained overload.** At 921600 flash needs ~2.2 s of work per 1 s of data, so loss is expected and counted.
- **The real burst fix is cheaper flash work** (batching, small records), not bigger buffers.
- **Static allocation only, ~8 KB of RAM.** No heap after start-up (firmware rule).

### 5. Noise handling: detect, drop, resync (no repair)

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
