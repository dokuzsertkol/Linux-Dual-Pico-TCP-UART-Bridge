# Linux Dual Pico 2 W TCP-UART Communication Bridge

A bidirectional communication system built with two Raspberry Pi Pico 2 W boards and a Linux server, combining TCP/IP networking and UART serial communication.

## Overview

The purpose of this project is to establish a reliable, bidirectional data transmission pipeline between a Linux server and two Raspberry Pi Pico 2 W boards.

The system uses TCP over Wi-Fi for communication between the Linux server and the first Pico, while UART provides a serial communication link between the two Pico boards.

### System Architecture

```text
+-------------+  Wi-Fi / TCP  +----------------+
| Linux Server|  __ __ __ __  |   Pico 2 W #1  |
+-------------+               +----------------+
      |                               |
      |                               |
      |                         UART Serial
      |                               |
      |                               |
      |          Wi-Fi / TCP   +----------------+
      |__ __ __ __ __ __ __ __ |   Pico 2 W #2  |
                               +----------------+
```

The system supports two communication paths:

- **TCP to UART:** Data received from the Linux server by Pico 1 is forwarded to Pico 2 through UART.
- **UART to TCP:** Data received from Pico 1 through UART is forwarded to the Linux server over TCP.

## Project Goals

The primary goal is to investigate the performance limitations of a multi-layer communication system and implement solutions to minimize data loss and maximize throughput.

The project focuses on:

- Reliable bidirectional data transmission.
- TCP socket programming on Linux.
- TCP communication using the lwIP stack on embedded devices.
- UART communication between microcontrollers.
- DMA-based data reception.
- Buffer management and flow control.
- Identifying and eliminating communication bottlenecks.
- Measuring throughput and monitoring transmission statistics.

## Technical Challenges and Bottlenecks

A major part of this project involves identifying bottlenecks that emerge when multiple communication interfaces operate at different speeds.

### 1. UART Throughput

UART provides a serial communication channel between the two Pico boards. Its transmission speed is limited by the configured baud rate.

At 1 Mbps, the theoretical maximum throughput is lower than 1 Mbps when measured in useful payload bytes because UART typically uses additional start and stop bits for each transmitted byte.

For standard 8N1 configuration:

- 1 start bit
- 8 data bits
- 1 stop bit

This results in 10 transmitted bits per byte.

Consequently, a 1 Mbps UART connection has a theoretical maximum payload throughput of approximately 100 KB/s, excluding additional protocol overhead.

### 2. CPU Overhead and DMA

Initially, UART data reception was handled using polling. Continuously checking the UART peripheral for incoming data introduces CPU overhead and can cause data loss when the CPU cannot process incoming bytes quickly enough.

To address this, DMA (Direct Memory Access) was introduced.

DMA transfers incoming UART data directly into a circular memory buffer without requiring the CPU to process every byte individually.

This allows the CPU to focus on parsing messages and forwarding data through TCP.

### 3. TCP Buffer Limitations

TCP communication introduces its own buffering constraints.

The lwIP stack manages transmission and reception through several buffers and queues, including:

- TCP send buffer (`TCP_SND_BUF`)
- TCP receive window (`TCP_WND`)
- TCP segment allocation (`MEMP_NUM_TCP_SEG`)
- Packet buffers (`PBUF_POOL_SIZE`)

When the application attempts to transmit data faster than the network stack can process it, TCP transmission requests may fail because of insufficient available buffer space.

The implementation therefore needs to account for buffer availability and handle transmission errors without losing data.

### 4. Communication Speed Mismatch

The system combines Wi-Fi/TCP and UART, which have different throughput characteristics.

When the incoming data rate exceeds the forwarding interface's processing capacity, buffers gradually fill up.

For example, if TCP delivers data faster than UART can transmit it, the application must prevent buffer overflow and preserve the integrity of the data stream.

This makes buffer management and transmission scheduling essential parts of the system.

### 5. Data Framing and Parsing

TCP and UART are byte streams rather than message-oriented protocols.

A single TCP read or UART reception event does not necessarily correspond to a complete application message.

Therefore, the application uses message framing and parsing logic to identify message boundaries and reconstruct complete messages from incoming byte streams.

## Performance Monitoring

The Linux server and Pico clients include transmission and reception statistics to observe system behavior.

The measurements include:

- Total messages sent and received.
- Total bytes transmitted and received.
- Transmission and reception throughput.
- Connection status.
- Communication errors.

Throughput measurements use a rolling one-second measurement interval, while statistics are periodically printed for monitoring.

## Technologies

| Component | Technology |
|---|---|
| Microcontrollers | 2 × Raspberry Pi Pico 2 W |
| Server | Linux |
| Networking | Wi-Fi, TCP/IP |
| Embedded networking | lwIP |
| Serial communication | UART |
| Hardware acceleration | DMA |
| Embedded development | Raspberry Pi Pico SDK |
| Programming language | C |
