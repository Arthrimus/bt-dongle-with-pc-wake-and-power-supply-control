# Pico USB Bluetooth HCI Wake Dongle (BC250 Optimized With PSU Control)

Firmware for both Raspberry Pi **Pico W** (RP2040) and **Pico 2 W** (RP2350) that acts as a USB Bluetooth HCI dongle and PC wake controller. The main path uses the onboard CYW43 Bluetooth controller:

```text
PC USB host -> Pico TinyUSB Bluetooth HCI device -> onboard CYW43 Bluetooth controller
```

The original Pico + external ESP32 UART-HCI backend is still available, but it is now a secondary hardware path documented in [ESP32 HCI UART Backend](docs/esp32_hci_uart.md).

## Supported Boards

| Board | Chip | Wireless |
|---|---|---|
| Raspberry Pi Pico W | RP2040 | CYW43439 WiFi+BT |
| Raspberry Pi Pico 2 W | RP2350 | CYW43439 WiFi+BT |

## What It Does

- Exposes a native USB Bluetooth HCI controller using TinyUSB's BTH device class.
- Bridges USB HCI command, event, and ACL traffic to the Pico W onboard CYW43 Bluetooth controller.
- Uses standby HCI host mode to scan for Bluetooth/BLE wake signals while the PC is off or asleep.
- Can pulse a PC power-button circuit for wake.
- Can drive a PS_ON pin low for direct power supply control.
- Accepts power control input from a momentary pushbutton.
- Supports a buzzer for audio feedback during power on events.
- Can expose a standby USB HID keyboard and send an F13 keypress for S3 wake.
- Persists learned BLE wake peers in flash and lets you clear that allowlist by holding BOOTSEL.

SCO/iso endpoints are left in the USB descriptor because TinyUSB's premade BTH descriptor includes the companion interface expected by the Bluetooth USB transport shape. The bundled TinyUSB BTH class exposes command, ACL, and event callbacks/APIs; this project keeps packet parsing SCO-aware, but full SCO data pass-through will need either a TinyUSB BTH extension or a local BTH class driver with isochronous transfer callbacks.

## Hardware

This firmware supports both the **Raspberry Pi Pico W** (RP2040) and **Pico 2 W** (RP2350). The onboard wireless chip is connected internally on both boards.

### GPIO Reservations

| Board | Reserved Pins | Purpose |
| --- | --- | --- |
| Pico W / Pico 2 W | `23`, `24` | CYW43 WiFi/BT controller |
| Pico 2 W only | `29` | CYW43 BT auxiliary (not on RP2040) |

Do not use reserved pins for front-panel wiring.

### Recommended Pins

| Signal | Pico GPIO | Notes |
| --- | ---: | --- |
| Power-button Output | 14 | Released as input with pull-up; press briefly drives low, matching a switch that shorts 3.3 V to ground. |
| PC power sense | 13 | Input with pulldown; reads 3.3 V as PC-on. |
| USB VBUS sense | -1 | Disabled by default; USB is assumed present. |
| Status LED | Onboard LED (auto) | Blinks while standby wake is waiting to arm, solid while scanning, off in dongle mode. |
| PS_ON | 12 | Released as input with pull-up; drives low to turn on the power supply when connected to the power supply's PS_ON pin. To protect the GPIO pin from the power supply's internal pullup use a diode or a logic buffer between the GPIO and the PS_ON pin |
| Momentary Pushbutton | 11 | Input with pullup. Pushbutton connected to ground reads as a power button press |
| Buzzer Output | 15 | Beeps a buzzer when a wake event occurs. Provides audible feedback when powering on |
| External Power Status LED | 10 | LED indicator for system power state. LED lit when PC is on, LED unlit when PC is off |

## Build

### Board Selection

Choose the target board with `-DPICO_BOARD_TYPE=<type>`:

| Option | Target | Chip |
| --- | --- | --- |
| `PICO_W` (default) | Raspberry Pi Pico W | RP2040 |
| `PICO_2_W` | Raspberry Pi Pico 2 W | RP2350 |

### Examples

**Pico 2 W front-panel build:**

```powershell
cd C:\Users\zheku\pico-usb-bt-wake
cmake -S . -B build-pico2w -G Ninja -DHCI_BACKEND=cyw43 -DPICO_BOARD_TYPE=PICO_2_W -DPIN_PWR_BUTTON_OUT=14 -DPIN_PWR_OK_SENSE=13 -DPIN_USB_VBUS_SENSE=-1 -DPIN_MOMENTARY_BUTTON=11 -DPIN_PS_ON_CONTROL=12 -DPIN_BUZZER=15 -DPIN_POWER_INDICATOR_LED=10 -DBUZZER_BEEP_MS=500u -DSTANDBY_WAKE_ARM_DELAY_MS=5000u -DMOMENTARY_BUTTON_LONG_PRESS_MS=15000u
cmake --build build-pico2w
```

**Pico W front-panel build (default):**

```powershell
cmake -S . -B build-picow -G Ninja -DHCI_BACKEND=cyw43 -DPICO_BOARD=pico_w -DPIN_PWR_BUTTON_OUT=14 -DPIN_PWR_OK_SENSE=13 -DPIN_USB_VBUS_SENSE=-1 -DPIN_MOMENTARY_BUTTON=11 -DPIN_PS_ON_CONTROL=12 -DPIN_BUZZER=15 -DPIN_POWER_INDICATOR_LED=10 -DBUZZER_BEEP_MS=500u -DSTANDBY_WAKE_ARM_DELAY_MS=5000u -DMOMENTARY_BUTTON_LONG_PRESS_MS=15000u
cmake --build build-picow
```

**Linux USB Bluetooth descriptor bring-up:**

```powershell
cmake -S . -B build-linux-test -G Ninja -DHCI_BACKEND=cyw43 -DPICO_BOARD_TYPE=PICO_2_W -DPIN_PWR_OK_SENSE=-1 -DPIN_PWR_BUTTON_OUT=-1 -DENABLE_POWER_BUTTON_WAKE=OFF
cmake --build build-linux-test
```

**Descriptor-only firmware (stub backend):**

```powershell
cmake -S . -B build-stub -G Ninja -DHCI_BACKEND=stub -DPICO_BOARD_TYPE=PICO_2_W
cmake --build build-stub
```

If the Pico SDK is not found automatically, set `PICO_SDK_PATH`.

## Useful Options

### Board & Backend

- `-DPICO_BOARD_TYPE=PICO_W` or `PICO_2_W` to select the target board; defaults to `PICO_W`.
- `-DHCI_BACKEND=cyw43` for the Pico W / Pico 2 W onboard controller.
- `-DHCI_BACKEND=stub` for descriptor-only bring-up.
- `-DSYS_CLOCK_KHZ=200000` to run the RP2350 system clock at 200 MHz; default `0` keeps the board/SDK clock.
- `-DENABLE_CDC_DEBUG=ON` to expose USB CDC debug serial.
- `-DENABLE_POWER_BUTTON_WAKE=ON` to allow automatic PC power-button pulses.
- `-DENABLE_STANDBY_HID_KEYBOARD=ON` to expose a standby USB keyboard wake interface, enabled by default.
- `-DENABLE_CYW43_STATUS_LED=ON` to use the Pico W onboard LED for standby status, enabled by default for CYW43 builds.
- `-DSTANDBY_WAKE_ARM_DELAY_MS=5000` to delay standby Bluetooth scanning and wake pulses after PC-off detection; default is 5 seconds.
- `-DSTATUS_LED_BLINK_MS=1000` to set the status LED blink half-period while standby wake is waiting to arm.
- `-DMOMENTARY_BUTTON_LONG_PRESS_MS=10000u` to set the duration of the pushbutton hold for hard power supply shutdown (default 10 seconds).
- `-DBUZZER_BEEP_MS=100` to set the beep duration in milliseconds (default 100ms).
- `-DSTANDBY_HID_WAKE_KEY=0x68` to choose the USB HID usage sent on wake; default is F13.
- `-DENABLE_ACL_DEBUG_LOG=ON` for verbose ACL packet logging during transport debugging.
- `-DWAKE_ON_KNOWN_BLE_PEER=ON` to wake when a BLE peer learned while the host was on advertises again, enabled by default.
- `-DWAKE_PERSIST_BLE_PEERS=ON` to keep learned BLE wake peers across power cycles, enabled by default.
- `-DENABLE_BOOTSEL_CLEAR_WAKE_PEERS=ON` to clear learned BLE wake peers by holding BOOTSEL, enabled by default.
- `-DWAKE_ON_BLE_DIRECTED_ADV=ON` to wake on BLE directed advertisements, enabled by default.
- `-DWAKE_ON_STADIA_ADV=ON` to wake on Stadia Controller BLE advertisements/scan responses, enabled by default.
- `-DWAKE_ON_CONNECTABLE_BLE_ADV=ON` to wake on any connectable BLE advertisement, noisy but useful for testing.
- `-DWAKE_ON_BLE_HID=ON` to wake on BLE HID service/appearance advertisements.
- `-DWAKE_ON_BLE_CONTROLLER_NAME=ON` to wake on controller-like BLE local names.
- `-DWAKE_ON_ANY_BLE_ADV=ON` for broad BLE wake testing.
- `-DPIN_PWR_OK_SENSE=<gpio>` for the PC-on sense input.
- `-DPIN_USB_VBUS_SENSE=<gpio>` for optional host VBUS sense.
- `-DPIN_PWR_BUTTON_OUT=<gpio>` for the isolated power-button pulse output.
- `-DPIN_PS_ON_CONTROL=<gpio>` for ATX PS_ON control (active low).
- `-DPIN_MOMENTARY_BUTTON=<gpio>` for momentary power button wake input.
- `-DPIN_LED_STATUS=<gpio>` for status LED output.
- `-DPIN_LED_FAULT=<gpio>` for fault LED output.
- `-DPIN_BUZZER=<gpio>` for piezoelectric buzzer output (or -1 to disable, default).


The ESP32 UART backend has its own wiring and build options in [ESP32 HCI UART Backend](docs/esp32_hci_uart.md).

### Build Examples

For the normal Pico W front-panel build:

```powershell
cd C:\Users\zheku\pico-usb-bt-wake
cmake -S . -B build-picow -G Ninja -DHCI_BACKEND=cyw43 -DPICO_BOARD_TYPE=PICO_W -DPIN_PWR_BUTTON_OUT=10 -DPIN_PWR_OK_SENSE=11 -DPIN_USB_VBUS_SENSE=-1
cmake --build build-picow
```

For the normal Pico 2 W front-panel build:

```powershell
cd C:\Users\zheku\pico-usb-bt-wake
cmake -S . -B build-pico2w -G Ninja -DHCI_BACKEND=cyw43 -DPICO_BOARD_TYPE=PICO_2_W -DPIN_PWR_BUTTON_OUT=10 -DPIN_PWR_OK_SENSE=11 -DPIN_USB_VBUS_SENSE=-1
cmake --build build-pico2w
```

## Wake Behavior

By default, `PWR_OK` and USB VBUS are assumed present when their pins are `-1`, which makes bench USB dongle bring-up easier. The Pico 2 W front-panel build enables automatic power-button wake when `PIN_PWR_BUTTON_OUT` is set: it releases the pin with pull-up, simulates a press by driving it low for 200 ms, then waits 10 seconds before trusting the power LED/sense input again.

After the firmware detects that the PC is off, it waits `STANDBY_WAKE_ARM_DELAY_MS` before starting standby Bluetooth detection or allowing another wake pulse. The default is 60 seconds, which avoids immediately waking the PC again during shutdown or reboot transitions.

The Pico 2 W onboard LED is off in normal USB Bluetooth dongle mode, slow-blinks during that standby arm delay, and stays on once standby Bluetooth scanning is active.

When `ENABLE_STANDBY_HID_KEYBOARD=ON`, the USB device stays connected in standby as a composite Bluetooth HCI plus boot-keyboard device with remote wake advertised. The Bluetooth bridge is disabled while the Pico owns the controller for standby scanning, and a matching wake event requests USB remote wake plus an F13 key press. If HID wake is disabled, the older behavior is used: when the power LED/sense input reads low, the firmware detaches USB while it runs the standby HCI scanner. For pure USB dongle testing, either hold the sense pin high or build with `-DPIN_PWR_OK_SENSE=-1`.

The default standby BLE wake policy targets devices trying to return to this adapter: classic Bluetooth connection requests, BLE directed advertisements, BLE advertisements from peers learned during earlier host-side BLE connections, and Stadia Controller advertisements whose local name starts with `Stadia`. Learned BLE peers are stored in flash so they survive power cycles. For a strict saved-address-only Stadia wake policy, disable `WAKE_ON_STADIA_ADV` after the controller has been learned. Broader BLE matching is available through the wake options above.

Hold the Pico BOOTSEL button for about 5 seconds to clear the persisted BLE wake peer list. This does not clear Windows or BlueZ Bluetooth pairing keys; it only resets the Pico's standby wake allowlist.

### PS_ON Control

When `PIN_PS_ON_CONTROL` is configured with a GPIO pin number, the firmware directly controls the ATX power supply enable signal (active-low) for more reliable power management than relying solely on the power button circuit.

**How it works:**

1. **Power-On Assertion:** When a wake event occurs (button press, BLE peer detected, HCI host command) while PWR_OK is low, the firmware asserts PS_ON LOW *before* pulsing the power button. This ensures the PSU receives power before the motherboard sees the button press, avoiding missed presses during fast boot scenarios.

2. **Shutdown Grace Period:** After asserting PS_ON LOW during wake, if PWR_OK never goes high (indicating the PC didn't boot), the firmware starts a 5-second grace timer. Once expired, PS_ON is released HIGH to cut power cleanly. If PWR_OK goes high during this period, the timer cancels.

3. **Force Shutdown Detection:** When both `PIN_PS_ON_CONTROL` and `PIN_MOMENTARY_BUTTON` are configured, holding the momentary button for `MOMENTARY_BUTTON_LONG_PRESS_MS` (default 10 seconds) forces PS_ON HIGH — an emergency power-off that overrides normal operation.

**Example build with PS_ON control:**

```powershell
cmake -S . -B build-picow -G Ninja -DHCI_BACKEND=cyw43 \
    -DPIN_PS_ON_CONTROL=12 \
    -DPIN_MOMENTARY_BUTTON=13 \
    -DMOMENTARY_BUTTON_LONG_PRESS_MS=5000
cmake --build build-picow
```

This configuration connects PS_ON to GPIO 12, the momentary button to GPIO 13, and sets the force-shutdown hold time to 5 seconds.

## Debug BOOTSEL Command

Debug CDC builds accept a serial command that jumps straight into the Pico USB bootloader:

```powershell
$p = New-Object System.IO.Ports.SerialPort 'COM15',115200,'None',8,'One'
$p.DtrEnable = $true
$p.RtsEnable = $true
$p.Open()
$p.WriteLine('bootsel')
Start-Sleep -Milliseconds 200
$p.Close()
```

Use the current debug COM port in place of `COM15`. The aliases `bootsel` and `boot` both work.

Debug CDC builds also enable the Pico SDK 1200-baud reset hook:

```powershell
$p = New-Object System.IO.Ports.SerialPort 'COM15',1200,'None',8,'One'
$p.Open()
Start-Sleep -Milliseconds 200
$p.Close()
```

## References

- [Hardware wiring](docs/hardware_wiring.md)
- [Linux HCI transport reference](docs/linux_hci_transport_reference.md)
- [Linux test plan](docs/linux_test_plan.md)
- [Standby HCI host mode](docs/standby_hci_host_mode.md)
- [Troubleshooting](docs/troubleshooting.md)
