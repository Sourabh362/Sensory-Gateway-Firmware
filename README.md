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