# SwarmForge – ESP8266 ESP-NOW Communication Demo

A simple prototype communication layer for the SwarmForge warehouse swarm robot system using two ESP8266 boards.

## Architecture

```text
Laptop / Browser
      │
      │ Wi-Fi / HTTP
      ▼
Master ESP8266
      │
      │ ESP-NOW
      ▼
Slave ESP8266
```

* **Master:** Creates Wi-Fi AP, hosts web server, sends commands, tracks ACKs and Slave status.
* **Slave:** Receives commands, simulates robot states, and sends ACK + status.
* **Dashboard:** Browser-based interface for commands, status, statistics, and event logs.

> No motors, sensors, navigation, or physical robot movement are used. Robot movement is simulated in software.

## Hardware

* 2 × ESP8266
* 2 × USB cables
* Laptop/PC

## Software

* Arduino IDE
* ESP8266 board package **3.1.2**
* Board: **NodeMCU 1.0 (ESP-12E Module)**

No additional libraries are required.

## Project Structure

```text
SwarmForge_Communication/
├── Master/
│   └── Master.ino
├── Slave/
│   └── Slave.ino
└── dashboard.html
```

## Configuration

Wi-Fi:

```text
SSID: SwarmForge
Password: swarm1234
Master IP: 192.168.4.1
ESP-NOW Channel: 1
Serial: 115200 baud
```

The Slave MAC address must be entered in `Master.ino`.

## Setup

1. Upload `Slave.ino` first.
2. Open Serial Monitor at `115200`.
3. Copy the Slave MAC address.
4. Enter the MAC address in `Master.ino`.
5. Upload `Master.ino`.
6. Power both ESP8266 boards.
7. Confirm the Master shows `SLAVE FOUND`.
8. Connect the laptop to the `SwarmForge` Wi-Fi network.
9. Open:

```text
http://192.168.4.1/status
```

10. Open `dashboard.html`.

## Commands

The dashboard supports:

* MOVE FORWARD
* MOVE BACKWARD
* TURN LEFT
* TURN RIGHT
* GO TO ZONE A
* GO TO ZONE B
* STOP

Movement commands are simulated for approximately **5 seconds**, after which the Slave returns to `IDLE`.

## Communication

Commands use:

```text
CommandPacket
├── packetId
└── command
```

Responses use:

```text
StatusPacket
├── packetId
├── acknowledged
└── status
```

The Master waits up to **2 seconds** for an ACK.

A heartbeat `PING` is sent every **2 seconds**, and the Slave is considered offline after **7 seconds** without a response.

## Dashboard

The dashboard displays:

* Master online/offline status
* Slave online/offline status
* Current command
* Slave state
* ACK status
* Commands sent
* ACKs received
* Failed commands
* Communication event log

## Demo Flow

```text
Dashboard
    ↓
Master receives command
    ↓
ESP-NOW packet
    ↓
Slave processes command
    ↓
Slave sends ACK + status
    ↓
Master updates status
    ↓
Dashboard displays result
```

## Limitations

* One Master + one Slave
* Slave MAC is hardcoded
* No real robot movement
* No sensors or navigation
* Fixed ESP-NOW channel
* No encryption configured
* No automatic command retries
* Dashboard uses HTTP polling

## Purpose

This prototype demonstrates the basic communication building block for a future SwarmForge multi-robot system, where the Master can be extended to communicate with multiple robots.
