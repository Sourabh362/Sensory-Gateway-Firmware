# How the Gateway Works

How to test it, how it works, and why it is built this way.
Full reasoning for each decision: [decisions.md](decisions.md).

---

## 1. How to test

```sh
make test                      # everything: 11 tests, ~25 s
./build/test_parser            # run one test program and see each check
```

| Test | What it proves | How |
|---|---|---|
| `test_spsc_ring` | The ISR → main-loop handoff is safe | Basic cases, plus **2 real threads passing 10M numbered items**: none lost, duplicated or reordered |
| `test_parser` | Every intact frame is found in noise | The brief's example frames, each noise type, and a 200 KB random noisy stream where every intact frame must appear in order |
| `test_flash_sim` | The fake flash behaves like real NOR | 0→1 writes rejected, erase, timing, survives reopening, cut mid-write and mid-erase |
| `test_flashlog` | Power loss never loses stored data | Boot cases, plus **1,500 random power cuts**. After each reboot, every reading reported as stored must still be there |
| `e2e_samples_*` | The whole system meets §5.6 of the brief | Replays Fluxton's `.bin` at the real baud and compares `stats` with the `.json` manifest |
| `e2e_samples_generated_*` | Same, on extra cases | Our own captures (adds unknown-type frames) |

**Manual checks (review demo):**

```sh
rm -f flash.img
./build/gateway --capture samples/noisy.bin --exit-when-done                        # run + stats
./build/gateway --capture samples/noisy.bin --cut-after-bytes 5003 --exit-when-done  # pull the plug
./build/gateway --capture samples/noisy.bin                                          # reboot → "dump 3", "stats"
```

---

## 2. The big picture

```
              INTERRUPT CONTEXT                 MAIN LOOP (stages)
          ┌───────────────────┐
 .bin ───►│ UART RX ISR       │──► [rx ring 4 KB] ──► PARSE ──► [reading queue 512] ──► SAVE ──► flash log ──► NOR
          │ count rx / lost   │                      stage                              stage
          └───────────────────┘                                                         ▲
          ┌───────────────────┐                                                         │
          │ 1 ms tick ISR     │──► uptime, flash timeouts           [cmd queue] ────────┘
          └───────────────────┘                                          ▲
 stdin ──► console ───────────────────────────────────────────► CONSOLE stage (stats / dump / erase)
```

| Folder | Role |
|---|---|
| [include/hal.h](../include/hal.h) | The only door to hardware |
| [core/](../core/) | The firmware: portable, no OS calls, no heap |
| [sim/](../sim/) | Fake hardware: threads play the interrupts, a file plays the flash |
| [tests/](../tests/), [tools/](../tools/) | Unit tests, power-cut torture test, end-to-end checks |

| File | Responsibility |
|---|---|
| `core/spsc_ring.c` | Lock-free single-producer/single-consumer queue (bytes, readings, commands) |
| `core/crc16.c` | CRC-16/CCITT-FALSE |
| `core/parser.c` | Byte-by-byte frame state machine with rescan recovery |
| `core/reading.c` | The 8-byte reading/record: decode, CRC validation |
| `core/flashlog.c` | Power-safe log: sectors, records, batching, boot recovery |
| `core/app.c` | ISRs, the three stages, console commands, `stats` |
| `sim/sim_uart.c` | Thread that replays a capture at the baud rate (plays the UART interrupt) |
| `sim/sim_tick.c` | Thread that fires every 1 ms (plays the timer interrupt) |
| `sim/sim_flash.c` | 64 KB NOR in a file: timing, 1→0 rule, power-cut switch |
| `sim/sim_console.c` | stdin reader → non-blocking line mailbox |
| `sim/main.c` | Options, boot, super loop |

---

## 3. The working procedure: the life of one byte

### Step 1: the byte arrives (ISR), in `core/app.c`

```c
void app_uart_rx_isr(uint8_t byte) {
    counter_inc(&bytes_received);
    if (!spsc_push(&rx_ring, &byte))   // never waits
        counter_inc(&bytes_lost);      // full → drop + count
}
```

- The sim's UART thread calls this once per byte at baud ÷ 10 bytes/s (8N1).
- On a real MCU, the UART interrupt would call it after reading the (already volatile) data register.
- **Trade-off:** the ISR does the bare minimum (count + push). All parsing happens later, so the ISR stays short and can never block.

### Step 2: the handoff (SPSC ring), in `core/spsc_ring.c`

```
push:  read tail (acquire) → full? → copy byte into slot → publish head+1 (release)
pop:   read head (acquire) → empty? → copy byte out     → publish tail+1 (release)
```

- **Only the ISR writes `head`, and only the main loop writes `tail`.** No locks are needed.
- **Release/acquire ordering:** if the reader sees the new `head`, the byte is guaranteed to already be in the slot.
- **The indices count up forever:** `count = head - tail`, slot = `index & mask`. Unsigned wrap at 2³² works automatically (tested).
- **The UART register is volatile, but the ring is plain RAM.** Its safety comes from atomics, not from `volatile`.
- **Trade-off:** C11 atomics instead of `volatile`. They are explicit, also correct on multi-core and in the simulation (where the ISR is a real thread), and free on Cortex-M; the cost is slightly more verbose code.

### Step 3: parsing, in `core/parser.c`

`app_parse_step()` pops up to 256 bytes per pass and feeds each one to the state machine:

```
SOF ──AA──► LEN ──≤64──► TYPE ──► PAYLOAD (LEN bytes) ──► CRC_LO ──► CRC_HI ──► check
 ▲   other: discard   >64: REJECT                                       CRC bad: REJECT
 └──────────────────────────────────────────────────────────────────────── CRC ok: done
```

| Result | Action | Counter |
|---|---|---|
| CRC OK, type `01`, LEN 6 | Reading → callback → reading queue | `readings` |
| CRC OK, type `02`, LEN 0 | Heartbeat, counted only | `heartbeats` |
| CRC OK, anything else | Ignored | `unknown_frames` |
| LEN > 64 | REJECT | `len_errors` |
| CRC wrong | REJECT | `crc_errors` |
| Byte ≠ `AA` while hunting | Discarded | `bytes_discarded` |

**On REJECT: drop only the first byte (the false `AA`) and feed every byte collected after it back through the state machine.**

```
noise "AA 05" then real frame "AA 06 01 29 09 E8 03 00 00 7B 84"
→ parser thinks LEN=5, swallows the real frame's start, CRC fails
→ drop the first AA, replay "05 AA 06 01 ..." → finds the real frame ✓
```

- At most 69 bytes (one max frame) are ever in flight, so a fixed buffer is enough (no heap).
- **Trade-off:** damaged frames are detected and dropped, never repaired. A wrong stored value is worse than a missing one.

### Step 4: the reading queue

- `on_reading()` pushes the reading (8 bytes: payload + the frame's own CRC) into the 512-entry queue.
- If the queue is full, the **whole reading** is dropped and counted (`readings_dropped`). The parse stage never waits.
- **Trade-off:** under overload we lose whole readings, never raw UART bytes. Lost bytes would break frames and blur the stats. In practice drops are 0, even for burst.

### Step 5: saving, in `core/app.c` → `core/flashlog.c`

`app_save_step()` does **one unit of flash work per pass**, in priority order:
erase-all step → dump chunk → new command → write readings.

```c
n = spsc_peek(&reading_q, batch, 32);     // look, don't remove
stored = flashlog_append(&flog, batch, n);
spsc_release(&reading_q, stored);          // remove ONLY what reached flash
```

Inside `flashlog_append`:

1. **Active sector full?** `open_next_sector()` takes the next sector in ring order. If it holds old data, count those readings as discarded, then **erase → write header → continue**.
2. **Write up to 32 records in one write**, never crossing a 256 B page.
3. **Wait until flash isn't busy**, with a timeout (the firmware never hangs).
4. **Return how many were stored.** Only now are they "stored".

- **Trade-off:** batching. At low rates each reading is written immediately; when a backlog builds, 32 go in one write. That is how burst reaches zero loss: ~34% flash busy instead of over 100%.

### What is in flash

```
Sector (4 KB): [header 8 B: magic | sector seq | CRC][rec][rec]...[rec]   (511 records)
Record (8 B):  [temp 2][timestamp 4][frame CRC 2]
```

- **That is all.** No counters and no write pointer are stored in flash.
- **Everything else lives in RAM** and is rebuilt at boot.

---

## 4. Power loss: what happens

**A record is valid only if `crc16(06 01 + payload) == stored CRC`.** A blank slot (all `FF`) can never pass: `crc16(06 01 FF×6) = 0x9E4B`.

| Power dies... | What's left in flash | At boot |
|---|---|---|
| Mid-record | Some bytes written, rest `FF` | CRC fails → skipped, slot never reused. Never reported as stored, so nothing promised is lost |
| After the write completed | Full record | Valid ✓ |
| Mid-erase | Half-erased sector, broken header | Erased again. It held the oldest data, which was being discarded anyway |
| Between erase and header | Blank sector | Treated as free |
| Mid-header write | Non-blank, broken header | Erased again. No records were written yet |

**Boot recovery (`flashlog_init`):**

1. The valid header with the **highest sector seq** is the newest sector.
2. Walk backwards while the seqs are consecutive. That is the live chain.
3. Any non-blank sector outside the chain is garbage (torn erase/header), so erase it.
4. Count the valid records, and resume writing **after the last used slot** (torn slots are skipped).

- **Trade-off:** no stored pointer. A pointer would need an erase on every update, wear out its sector, and could itself be torn by a power cut. Scanning costs only milliseconds, since reads are free.

---

## 5. Main loop and console

```c
for (;;) {                                   // sim/main.c
    bool busy = app_parse_step() | app_save_step() | app_console_step();
    if (!busy) sim_sleep_us(100);            // like WFI on an MCU
}
```

- **`stats`** is printed directly by the console stage. All counters are atomic with a single writer, so any context can read them.
- **`dump n` and `erase`** are posted to the save stage through the command queue, because only it may touch flash.
- **Bounded work per pass:** dump prints 32 lines, erase does one sector. The UART ring keeps draining meanwhile.
- **Multi-core:** the same three stages could run on different cores, because every link between them is an SPSC queue and each resource has one owner.
- **Trade-off:** a super loop instead of an RTOS. It is simpler with no scheduler, and the stage design means moving to FreeRTOS tasks later only changes the glue code.

---

## 6. The simulator (fake hardware)

| Real hardware | Simulation |
|---|---|
| UART RX interrupt | Thread replaying the `.bin` at baud ÷ 10 bytes/s (8N1), delivered in ~100 µs FIFO-like bursts |
| SysTick / timer interrupt | Thread calling `app_tick_isr()` every 1 ms |
| NOR flash chip | 64 KB file via `mmap`: erase 20 ms, write 100 µs, 0→1 writes rejected, survives a hard kill |
| Power loss | `--cut-after-bytes N` / `--cut-on-erase K`: stop mid-write or mid-erase |
| Debug UART | stdin thread → one-line mailbox, stdout |

- **Moving to a real MCU replaces only `sim/`.** `core/` and `hal.h` stay the same.
- In tests, the power-cut hook jumps back into the test (`longjmp`) to "reboot" in-process; in `gateway` it exits the process.

---

## 7. Trade-offs at a glance

| Decision | Chosen | Rejected | Why |
|---|---|---|---|
| UART input | ISR + ring buffer | Polling in the loop | Polling loses bytes during a 20 ms erase |
| Concurrency | Lock-free SPSC, atomics | Mutex, disabling IRQs, `volatile` only | ISR must never block; correct on multi-core too |
| Pipeline | Stages + queues | Parse-then-save blocking | Parsing never stalls; overload drops whole readings, not bytes |
| Noise | Rescan after the false SOF | Skip the whole bad packet | A good frame can start inside a bad one |
| Damaged frames | Detect + drop + count | Repair via CRC | A wrong value is worse than a missing one |
| Record | 8 B: payload + frame CRC | 6 B (no check), 16 B (seq + commit) | Smallest size that still detects torn writes; one write per record |
| Position after reboot | Rebuilt by scanning | Pointer/counter in flash | A pointer wears out flash and can itself be torn |
| Erase | Just in time | Pre-erase one sector ahead | Full capacity; batching already keeps up |
| Writes | Batched (up to one page) | One write per reading always | 30× fewer writes under load, so burst has zero loss |
| Storage policy | Store every reading | Store on change | The brief requires it; every timestamp is unique |
| Scheduling | Super loop | RTOS | Simpler; stages port to RTOS unchanged |
