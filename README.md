# High-Throughput TCP/UART Bridge

A bidirectional embedded networking project built with two **Raspberry Pi Pico 2 W** boards and a **Linux TCP server**.

The primary goal was to achieve **lossless, maximum practical data transfer over UART at 1 Mbaud**, while transporting the data between computers and embedded devices through Wi-Fi and TCP.

```text
Linux server ◄────────── loopback result ───────────┐
     │ TCP                                          │
     ▼                                              │ TCP
TCP2UART Pico ─── UART at 1,000,000 baud ───► UART2TCP Pico

```

This project combines two independent paths:

- **TCP ─> UART:** the Linux server sends framed messages to the first Pico over Wi-Fi, and that Pico writes the messages to UART.
- **UART ─> TCP:** the second Pico receives UART data, reassembles application frames, and sends them back to the Linux server over Wi-Fi.

The return path allows the server to verify that every generated message arrived and to calculate the success rate.

## Objective

The difficult part was not simply moving bytes. It was making the whole pipeline keep up without overflow, frame corruption, or lost messages when it reached the UART bottleneck.

UART at 1 Mbaud has a maximum physical payload rate of approximately:

```text
1,000,000 bits/second ÷ 10 bits/byte = 100,000 bytes/second
```

The extra 2 bits per byte represent one start bit and one stop bit. With typical 8-N-1 UART framing, the theoretical maximum is 100 KB/s in bytes per second, not 125 KB/s.

The 7-byte application header also counts toward the transmitted size, so the wire throughput is divided among headers and payload:

| Payload message | Total frame size | Header overhead |
| --------------: | ---------------: | --------------: |
|       100 bytes |        107 bytes |           6.54% |
|       500 bytes |        507 bytes |           1.38% |

TCP and Wi-Fi are generally much more bursty than UART. The system therefore had to turn network bursts into a continuous, paced serial stream and handle large temporary differences in producer and consumer speeds.

## System Architecture

```text
┌──────────────────────────────────────┐
│ Linux TCP server                     │
│                                      │
│ • Generates counter-based messages   │
│ • Sends to TCP2UART client           │
│ • Reads UART2TCP client responses    │
│ • Detects gaps in counters           │
│ • Measures throughput and loss       │
└──────┬─────────────────┬─────────────┘
       │  TCP port 3000  │
       │                 │
       ▼                 ▼
┌──────────────┐   ┌──────────────┐
│ TCP2UART     │   │ UART2TCP     │
│ Pico 2 W     │   │ Pico 2 W     │
│ TCP -> UART  │   │ UART -> TCP  │
└──────┬───────┘   └──────┬───────┘
       └─── 1 Mbaud UART ─┘
```

The Pico at the two TCP boundaries have different bottlenecks:

- The **TCP2UART Pico** must absorb TCP bursts and feed a slower UART transmitter reliably.
- The **UART2TCP Pico** must consume UART bytes continuously and forward them back over Wi-Fi without losing RX bytes.

Both problems had to be solved without letting TCP or UART delivery boundaries determine application message boundaries.

## Message Format

The protocol is shared by the Linux server and both Pico applications.

```text
Byte offset  Size  Field
0            1     Message ID
1            2     Payload size, little endian
3            4     Counter, little endian
7            N     Payload
```

The fixed header size is 7 bytes:

```text
[ID][Payload Size 2 bytes][Counter 4 bytes][Payload bytes]
```

Six message types define fixed payload sizes:

| Message ID |   Payload |
| ---------- | --------: |
| `M_0B`     |   0 bytes |
| `M_100B`   | 100 bytes |
| `M_200B`   | 200 bytes |
| `M_300B`   | 300 bytes |
| `M_400B`   | 400 bytes |
| `M_500B`   | 500 bytes |

For example, `M_500B` produces a 507-byte frame.

The header is serialized explicitly instead of sending a packed C structure across the network. This makes the format independent of compiler padding, host architecture, and Pico/Linux endianness differences.

### Payload size and validation

Before parsing or serializing a message, the code checks:

- The message ID is valid.
- Payload size matches the message ID.
- Payload memory exists for a non-zero payload.
- The destination buffer is large enough.

This prevents malformed sizes and partially received frames from turning into out-of-bounds reads.

## Framing Byte Streams

TCP and UART are both byte streams. Neither transport guarantees that one application message arrives in one operation.

TCP may split a 507-byte message across several reads or may combine several messages in one read. UART may provide bytes through DMA one at a time or in chunks determined by software timing.

For this reason, every receiver assembles the frame using the following process:

```text
1. Accumulate bytes
2. Wait for 7 header bytes
3. Read the little-endian payload size
4. Validate the expected frame size
5. Wait until the full payload arrives
6. Deserialize the message
7. Reset the receive buffer
```

Application framing was one of the key solutions that made the bridge reliable. A socket receive boundary or DMA chunk can never accidentally be interpreted as a complete message.

## Bottleneck 1: UART RX Overflow

UART hardware can receive data faster than a normal application loop can parse it, especially when a receiver also performs TCP operations and diagnostic work.

A CPU that repeatedly calls `uart_getc()` and then processes the byte would risk dropping bytes during scheduling or LWIP callback activity.

### Solution: DMA ring-buffer reception

The UART2TCP Pico uses DMA to copy incoming UART bytes directly from the hardware UART data register into a 8192-byte ring buffer.

```text
UART peripheral ──DMA──> ring buffer ──application──> frame parser
```

The DMA configuration provides:

- 8-bit transfer size
- Incrementing write address
- Fixed UART receive-register source
- 8192-byte circular destination
- UART receive hardware request as the DMA trigger
- Interrupt handling to restart and continue reception

A single 8192-byte ring is also enough to absorb temporary delays in the application while it sends frames to TCP.

### Producer and consumer tracking

DMA writes to the ring buffer while the application consumes it. The implementation tracks:

- `produced`: next position written by DMA
- `uart_rx_consumed`: next position processed by the application

The produced position is derived from the DMA remaining transfer count:

```c
produced = (UART_RX_BUFFER_SIZE - remaining) & (UART_RX_BUFFER_SIZE - 1);
```

The application processes bytes until `uart_rx_consumed == produced`. It wraps both positions modulo 8192.

This separates hardware reception from application processing. The CPU does not have to serve every UART byte in the same interrupt as the hardware receives it.

## Bottleneck 2: TCP Produces Bursts Faster Than UART Sends

The Linux server can send TCP data much faster than UART can transmit it. Without buffering, the TCP2UART Pico would either block, return an error, or overwrite data it had not yet transmitted.

The UART is the slowest permanent stage in the pipeline:

```text
network arrival speed > UART transmission speed
```

TCP cannot be allowed to send a burst and then assume that every burst byte has already reached UART.

### Solution: non-blocking TCP reception with UART pacing

The TCP2UART Pico performs TCP callbacks through lwIP and writes complete frames to UART with `uart_write_blocking()`.

UART naturally regulates throughput. The blocking UART write is responsible for wait-free pacing because each UART byte can only complete as the hardware transmit FIFO accepts it.

The receive callback is written to assemble complete application frames before calling UART. Partial TCP frames are retained and combined with later TCP fragments.

The UART FIFO is enabled and stale bytes are flushed during startup.

## Bottleneck 3: UART and Wi-Fi Cannot Transmit at the Same Time Safely

In the TCP2UART Pico, there are two UART writers:

- The main code writes application frames received through TCP.
- Core 1 writes the heartbeat.

Concurrent writes could interleave their bytes and corrupt the application's byte stream.

### Solution: Pico mutex

The UART heartbeat and application writer share a Pico SDK `mutex_t`:

```text
TCP frame transmission ─┐
                        ├── UART mutex ──> UART hardware
Heartbeat transmission ─┘
```

Each write enters the mutex, writes the complete frame, and exits it. This prevents application data and heartbeat headers from being interleaved.

### Solution: dual-core heartbeat

Core 1 is dedicated to sending an `M_0B` heartbeat every 1000 ms. The heartbeat is independent of lwIP callback scheduling and lets the server detect whether the UART segment remains available.

A heartbeat does not carry a payload. It is exactly a 7-byte header.

## Bottleneck 4: lwIP TX Memory and TCP Callback Timing

The UART2TCP Pico receives UART frames much faster than a TCP callback can guarantee that lwIP will accept all of them. `tcp_write()` can fail with `ERR_MEM` when lwIP has insufficient output memory.

If the code discarded a complete frame after `ERR_MEM`, UART-to-TCP traffic would be lossy.

### Solution: bounded circular TCP transmit queue

The UART2TCP Pico stores up to 256 complete frames in a circular queue:

```text
UART frame ──> queue ──tcp_write()──> lwIP ──> Wi-Fi
                 ▲                      │
                 └──── sent callback ───┘
```

Each queue item contains:

- A complete serialized frame
- Its frame length

The queue maintains:

- `tx_head`: next item to send
- `tx_tail`: next insertion position
- `tx_count`: occupied items

Every successful `tcp_write()` advances the queue head only after lwIP accepts the bytes. The queue is therefore used to absorb the short-term difference between UART frame arrival and lwIP transmission capacity.

`TCP_WRITE_FLAG_COPY` causes lwIP to copy the application data instead of depending on a stack buffer that will disappear after the callback returns.

When lwIP reports a sent completion, `sent_callback()` flushes the queue again.

If the queue is full, the implementation records the frame as lost instead of silently overwriting older data.

## Bottleneck 5: TCP May Fragment or Combine Messages

LWIP may deliver data in `pbuf` structures that do not correspond to the server's application messages.

### TCP receive framing

The TCP2UART Pico and Linux server keep persistent receive buffers and do not assume that `read()` or `pbuf` boundaries correspond to message boundaries.

The Linux server processes each client with a separate receive buffer. The Pico TCP-to-UART path copies bytes from the lwIP `pbuf`, one at a time where required, and waits for complete frame lengths.

TCP receive callbacks also use lwIP state explicitly:

- `tcp_recved()` acknowledges accepted receive data.
- `pbuf_free()` releases completed packet buffers.
- `tcp_abort()` clears a connection after a fatal callback failure.
- Error and receive callbacks update reconnect state.

## Sequence Counter and Loss Verification

Maximum throughput is not useful if the receiver cannot prove that all bytes arrived. The project uses a 32-bit sequence counter in every payload-bearing message.

The Linux server increments the counter and sends messages from TCP2UART Pico to UART2TCP Pico and back.

When a message returns, the server compares it with the previous received counter:

```text
expected = previous_counter + 1

if (received_counter > expected)
    counters in [expected, received_counter - 1] are lost
```

The server reports:

```text
success rate = received messages / (received + missing) × 100
```

The counter is independent of TCP sequencing. A gap can identify a lost application frame even though TCP is already responsible for reliable delivery on its own connection.

The implementation currently does not add special handling for 32-bit counter wraparound. That is sufficient for the tested traffic range.

## TCP Client Registration

Each Pico sends a one-byte identity immediately after connecting:

|  ID | Role          |
| --: | ------------- |
| `0` | TCP2UART Pico |
| `1` | UART2TCP Pico |

The server stores them separately and can independently detect whether one or both clients are connected.

## Reconnection and Keepalive

The bridge should not become permanently unusable after a temporary network failure.

Both Pico applications use a reconnect state and retry connection setup every 2 seconds after lwIP reports failure.

TCP keepalive settings detect half-open or stalled connections sooner:

- Linux idle: 5 seconds
- Linux probe interval: 3 seconds
- Linux probe count: 5
- Linux user timeout: 3 seconds
- Pico idle: 1 second
- Pico probe interval: 3 seconds
- Pico probe count: 5

Keepalive does not increase UART reliability by itself. It detects that a TCP connection is no longer usable so the application can reconnect instead of using stale state.

## Live Measurement

The server measures both total transferred data and recent transfer rate:

- Total TCP bytes sent
- Total TCP bytes received
- Sent Kbps over a rolling window
- Received Kbps over a rolling window
- Total received messages
- Missing message counters
- End-to-end success percentage
- TCP2UART connection state
- UART connection state
- UART2TCP connection state

Both Pico programs also maintain counters and ring-buffered error records for:

- TCP receive or transmit corruption
- lwIP `pbuf` copy failure
- lwIP memory or queue failure
- UART receive overflow
- Message framing corruption
- UART transmission failure
- Buffer overflow

The latest four records in each error category include a message counter where possible, a byte count, and timestamp.

## Theoretical 1 Mbaud Limit

At 1 Mbaud using 8-N-1 UART framing:

```text
maximum UART byte rate = 1,000,000 / 10 = 100,000 bytes/second
maximum bitrate        = 800,000 payload bits/second
```

The 8-N-1 frame includes 8 data bits, a start bit, and a stop bit. The project used all fixed 100-byte increments for its payload-bearing test messages, so the maximum practical throughput is reduced by the 7-byte header and by firmware scheduling overhead.

The success goal was therefore not to exceed the physical UART limit. It was to **sustain the UART link continuously at 1 Mbaud without application losses** while making Wi-Fi bursts and lwIP callbacks behind it invisible to the serial stage.

## CMake Structure

The root CMake project builds two Pico targets:

- `tcp2uart-client`
- `uart2tcp-client`

The shared protocol is compiled as a static `protocol` library and linked into both firmware applications and the Linux server.

Pico target libraries include:

- `pico_stdlib`
- CYW43 lwIP TCP support
- Hardware UART support
- Hardware DMA support
- Pico multicore support
- The shared protocol library

A separate `linux-build-src/CMakeLists.txt` builds the server and protocol library on Linux.

## Building

### Linux server

```bash
cmake -S linux-build-src -B build-linux
cmake --build build-linux -j
./build-linux/server/server
```

The server listens on TCP port `3000` and writes detailed traffic to `output.txt`.

### Pico firmware

```bash
export PICO_SDK_PATH=/path/to/pico-sdk

cmake -S . -B build-pico -DPICO_BOARD=pico2_w
cmake --build build-pico -j
```

Build outputs:

```text
build-pico/tcp2uart-client/tcp2uart-client.uf2
build-pico/uart2tcp-client/uart2tcp-client.uf2
```

Flash each UF2 to the appropriate Pico 2 W board:

- `tcp2uart-client.uf2`: TCP receive, UART transmit
- `uart2tcp-client.uf2`: UART receive, TCP transmit

## Configuration

Update the Pico header files before deployment:

- Wi-Fi SSID
- Wi-Fi password
- Linux server IP address
- TCP server port
- UART TX pin
- UART RX pin
- UART baud rate

The current UART configuration is:

```text
UART:  uart0
TX:    GPIO 0
RX:    GPIO 1
Baud:  1,000,000
Mode:  8-N-1
```

Both Pico UART devices must use matching baud settings. RX and TX should cross-connect, and their grounds must be shared.

## Running the Complete Loop

1. Build and flash both Pico firmware images.
2. Connect the Pico UART TX/RX lines and ground.
3. Start the Linux server:

   ```bash
   ./build-linux/server/server
   ```

4. Connect both Pico boards to the configured Wi-Fi network.
5. Wait for client IDs `0` and `1` to connect.
6. Wait for UART heartbeat state to become connected.
7. Watch throughput, success rate, and lost counter ranges.

## Data Flow Example

For one 300-byte message with counter `42`:

1. The server creates an `M_300B` message.
2. `NewMessage()` stores 300 `X` bytes and counter `42`.
3. `MessageToBytes()` creates a 307-byte wire frame.
4. The server sends it to the TCP2UART Pico.
5. The Pico receives TCP fragments and reassembles the frame.
6. The Pico writes the frame to UART at 1 Mbaud.
7. DMA copies the bytes into the UART2TCP Pico's ring buffer.
8. The UART2TCP Pico reassembles the 307-byte frame.
9. It queues and sends the frame through lwIP/TCP.
10. The Linux server validates and counts counter `42`.

TCP protects each network leg from its own connection loss, while UART framing, DMA buffering, pacing, sequence counters, and diagnostics protect the full embedded pipeline from overflow and silent loss.
