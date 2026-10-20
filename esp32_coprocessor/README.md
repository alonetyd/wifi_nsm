# ESP32 Coprocessor - BT Controller Firmware

## Overview

This folder contains the firmware that needs to be flashed to your ESP32 coprocessor.

The ESP32-P4 cannot use Bluetooth directly (it has no BT hardware). It uses ESP-Hosted over SDIO to communicate with an ESP32 coprocessor that provides the Bluetooth controller.

## Setup Instructions

### Step 1: Set up ESP32 toolchain

You need to set up the ESP32 toolchain separately from the P4 toolchain:

```bash
# Install ESP32 support (if not already installed)
# The ESP32 toolchain should be different from P4's toolchain
```

### Step 2: Configure for ESP32 target

Navigate to this folder and configure for ESP32:

```bash
cd esp32_coprocessor

# Set ESP32 target
idf.py set-target esp32

# Or configure manually with defaults
idf.py reconfigure
```

### Step 3: Configure SDIO pins

Make sure the SDIO pins match your hardware connection between ESP32-P4 and ESP32:

- **ESP32-P4 side** (your current project):
  - CMD: GPIO 28
  - CLK: GPIO 31
  - D0: GPIO 27
  - D1: GPIO 26
  - D2: GPIO 30
  - D3: GPIO 29

- **ESP32 side** (this firmware) - Default pins:
  - SDIO CMD: GPIO 11
  - SDIO CLK: GPIO 6
  - SDIO D0: GPIO 7
  - SDIO D1: GPIO 8
  - SDIO D2: GPIO 9
  - SDIO D3: GPIO 10

If your ESP32 uses different pins, configure them in menuconfig:
```
Component config → ESP-Hosted CP → Transport → SDIO Configuration
```

### Step 4: Build and flash

```bash
# Build
idf.py build

# Flash to ESP32
idf.py -p /dev/ttyUSB0 flash monitor   # Replace with your ESP32's serial port
```

### Step 5: Verify ESP32 is running

You should see logs like:
```
I (XX) esp32_cp_bt: ===========================================
I (XX) esp32_cp_bt: ESP32 Coprocessor - BT Controller Firmware
I (XX) esp32_cp_bt: ===========================================
I (XX) esp32_cp_bt: NVS initialized
I (XX) esp32_cp_bt: BT controller ready, waiting for host...
```

## After ESP32 is flashed

Then rebuild and flash your ESP32-P4 project:

```bash
# Go back to P4 project
cd ..

# Reconfigure P4 project
idf.py reconfigure

# Rebuild
idf.py build

# Flash to ESP32-P4
idf.py flash monitor
```

## Troubleshooting

### HCI timeout errors on P4 side

If you see errors like:
```
E (6843) hci_bluedroid: hci tx failed (len=4)
E (14843) BT_HCI: command_timed_out hci layer timeout
```

1. Check that ESP32 is properly flashed and running
2. Verify SDIO wiring between P4 and ESP32
3. Check that ESP32 is receiving power
4. Try pressing the ESP32 reset button

### SDIO communication issues

Make sure:
- SDIO voltage levels match (both should be 3.3V)
- Wires are short enough for SDIO signals
- Ground is connected between both boards
