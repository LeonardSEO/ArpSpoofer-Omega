# 👾⚡ ArpSpoofer 4.0 "Omega"
### The Definitive Hardware-Level ARP Validation Engine for 2026

![Deployment Scenario](https://raw.githubusercontent.com/spacehuhn/enc28j60_ARPspoofer/master/images/1.jpg)

---

## 🌌 Overview
**ArpSpoofer 4.0 Omega** is a high-performance, production-grade firmware designed for the **Arduino Nano** and **ENC28J60** Ethernet controller. Engineered for network stress-testing and RFC-compliance validation, the Omega Engine transcends the limitations of legacy scripts by implementing industrial-standard protocols, advanced rate limiting, and autonomous state recovery.

> [!WARNING]
> **OPERATIONAL SECURITY NOTICE:** This firmware is for authorized lab use and educational purposes only. Deployment on unauthorized networks may violate local laws and corporate policies. Maintain physical isolation from production environments.

---

## 🚀 Key Innovations (Omega v4.0)

- **Ouroboros State Machine:** A robust logic core managing `INIT`, `DHCP`, `ANNOUNCE`, `RUN`, and `RECOVERY` states, ensuring 99.9% uptime on isolated segments.
- **Token-Bucket Rate Limiter:** Advanced flow control allowing for high-intensity bursts while maintaining a strict, user-defined average PPS (Packets Per Second).
- **RFC 5227 Compliance:** Implements the official IPv4 Address Conflict Detection announce sequence (3 GARPs with pseudo-random jitter).
- **Automatic PHY Recovery:** Real-time link-layer monitoring. If the connection is severed or the DHCP lease is revoked, the Engine automatically transitions to recovery mode.
- **EEPROM Intelligence:** Persistent storage for MAC addresses, PPS rates, and operational modes. Your tactical configuration survives power cycles.

---

## 🛠 Hardware Architecture

| Component | specification |
| :--- | :--- |
| **Microcontroller** | ATmega328P (Arduino Nano) |
| **Network Core** | Microchip ENC28J60 (SPI) |
| **Logic Voltage** | 3.3V / 5V (Level-shifted) |
| **Interface** | Serial CLI @ 115200 Baud |

![Module View](https://raw.githubusercontent.com/spacehuhn/enc28j60_ARPspoofer/master/images/2.jpg)

---

## ⚙️ Deployment Guide

### 1. Environment Setup
*   **IDE:** [Arduino IDE 2.x+](https://www.arduino.cc/en/software)
*   **Library Stack:** [EtherCard](https://github.com/jcw/ethercard)
*   **Drivers:** CH340/CH341 for Nano clones.

### 2. Firmware Integration
1.  Open `ArpSpoofer4.0-Omega.ino` in the IDE.
2.  Select **Arduino Nano** / **ATmega328P** (Use "Old Bootloader" if required).
3.  Compile and Flash.

---

## 🎮 Command & Control (Serial CLI)

The Omega Engine features an advanced real-time interface at **115200 baud**.

| Command | Action | Description |
| :--- | :--- | :--- |
| `R<n>` | **Set Rate** | Adjust PPS (1–500). Max burst handled by Bucket. |
| `S` | **Status** | Full diagnostic snapshot (Packets sent, State, IP, MAC). |
| `P` | **Pause** | Instant freeze/resume of the transmit engine. |
| `C` | **Commit** | Save current configuration to non-volatile EEPROM. |
| `M<suffix>` | **MAC Mod** | Patch the last 3 bytes of the MAC (e.g., `M1A2B3C`). |
| `?` | **Help** | Display the command matrix. |

---

## 🗺 Roadmap to 5.0
- [x] State Machine Overhaul
- [x] Token-Bucket Integration
- [x] EEPROM Persistence
- [ ] Multi-Target Cycle Mode
- [ ] Web-based Dashboard (ENC28J60 HTTP Stack)
- [ ] OUI Impersonation Tables

---

## 📜 Legal & Licensing
Distributed under the **MIT License**. Original development by **LeonardSEO**.
*Project references: RFC 5227, IEEE 802.3, Microchip Data Sheets.*

---
**Maintained by Janus & Tesavek 👾⚡**
