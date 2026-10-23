# How It Works: Visual Flow

## 1. Start-up

```mermaid
flowchart TD
    A([./build/gateway --capture noisy.bin]) --> B[main<br/>read options]
    B --> C[Load .bin file<br/>Open flash.img]
    C --> D[app_init: boot]
    D --> E[Start 1 ms tick]
    E --> F[Scan flash<br/>recover stored readings]
    F --> G[Print boot summary + prompt]
    G --> H[Start UART]
    H --> I((Super loop<br/>forever))

    style A fill:#e3f2fd,stroke:#1565c0
    style I fill:#e8f5e9,stroke:#2e7d32
    style F fill:#fff3e0,stroke:#ef6c00
```

## 2. Data path: from UART byte to flash

```mermaid
flowchart LR
    subgraph HW["Hardware (simulated by threads)"]
        U[UART<br/>byte arrives]
        T[1 ms tick]
        K[Keyboard<br/>typed line]
    end

    subgraph ISR["Interrupt context"]
        R[app_uart_rx_isr<br/>count + push]
    end

    subgraph LOOP["Main loop stages"]
        P[1. PARSE<br/>state machine<br/>CRC check]
        S[2. SAVE<br/>only flash owner]
        C[3. CONSOLE<br/>stats / dump / erase]
    end

    RX[(rx ring<br/>4 KB)]
    RQ[(reading queue<br/>512)]
    CQ[(cmd queue<br/>4)]
    FL[(flash.img<br/>64 KB NOR)]

    U --> R --> RX --> P
    P -- valid reading --> RQ --> S
    S -- batched write --> FL
    K --> C
    C -- dump / erase --> CQ --> S
    S -- dump reads --> FL
    T -. uptime, timeouts .-> S

    style RX fill:#fff3e0,stroke:#ef6c00
    style RQ fill:#fff3e0,stroke:#ef6c00
    style CQ fill:#fff3e0,stroke:#ef6c00
    style FL fill:#e8f5e9,stroke:#2e7d32
    style R fill:#fce4ec,stroke:#c2185b
```

## 3. One pass of the super loop

```mermaid
flowchart TD
    L([Loop start]) --> P{Bytes in<br/>rx ring?}
    P -- yes --> P1[Parse up to 256 bytes<br/>valid readings → reading queue]
    P -- no --> S
    P1 --> S{Save stage:<br/>what to do?}
    S -- erase job --> S1[Erase one sector]
    S -- dump job --> S2[Print up to 32 readings]
    S -- new command --> S3[Start dump / erase]
    S -- readings waiting --> S4[Write up to 32 readings<br/>in one flash write]
    S -- nothing --> C
    S1 --> C
    S2 --> C
    S3 --> C
    S4 --> C{Command<br/>typed?}
    C -- stats --> C1[Print counters]
    C -- dump / erase --> C2[Post to cmd queue]
    C -- no --> W
    C1 --> W
    C2 --> W
    W{Any work<br/>done?}
    W -- yes --> L
    W -- no --> Z[Sleep 100 µs<br/>like WFI] --> L

    style L fill:#e8f5e9,stroke:#2e7d32
    style Z fill:#eceff1,stroke:#546e7a
```

## 4. Parser: finding frames in noise

```mermaid
stateDiagram-v2
    [*] --> SOF
    SOF --> SOF: byte ≠ AA (discard)
    SOF --> LEN: AA
    LEN --> TYPE: LEN ≤ 64
    LEN --> REJECT: LEN > 64
    TYPE --> PAYLOAD: LEN > 0
    TYPE --> CRC_LO: LEN = 0
    PAYLOAD --> PAYLOAD: more bytes
    PAYLOAD --> CRC_LO: LEN bytes read
    CRC_LO --> CRC_HI
    CRC_HI --> GOOD: CRC OK
    CRC_HI --> REJECT: CRC wrong
    GOOD --> SOF: reading / heartbeat / unknown
    REJECT --> SOF: drop first AA,<br/>rescan the rest
```

## 5. Saving a reading and power-cut safety

```mermaid
flowchart TD
    A[Readings in queue] --> B{Active sector<br/>full?}
    B -- no --> E
    B -- yes --> C{Next sector<br/>has old data?}
    C -- yes --> D1[Drop oldest readings<br/>Erase sector 20 ms]
    C -- no --> D2[Already blank]
    D1 --> D3[Write sector header]
    D2 --> D3
    D3 --> E[Write up to 32 records<br/>one flash write]
    E --> F[Wait until flash not busy]
    F --> G([Reported as STORED<br/>remove from queue])

    style G fill:#e8f5e9,stroke:#2e7d32
    style D1 fill:#ffebee,stroke:#c62828
```

## 6. Boot after a power cut

```mermaid
flowchart TD
    A([Power on]) --> B[Read all 16 sector headers]
    B --> C[Newest = highest sector seq]
    C --> D[Walk back through<br/>consecutive seqs = live data]
    D --> E{Other sectors<br/>not blank?}
    E -- yes --> F[Erase them<br/>torn erase / header]
    E -- no --> G
    F --> G[Count valid records<br/>CRC check each]
    G --> H[Skip torn records]
    H --> I([Resume writing after<br/>last used slot])

    style A fill:#e3f2fd,stroke:#1565c0
    style I fill:#e8f5e9,stroke:#2e7d32
```
