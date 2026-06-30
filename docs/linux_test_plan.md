# Linux Test Plan

## Descriptor Bring-Up

Build with the CYW43 or stub backend and flash the UF2. For active Bluetooth descriptor testing, keep the power LED/sense pin high or disable it with `-DPIN_PWR_OK_SENSE=-1`; otherwise the firmware enters standby scan mode.

Use `ENABLE_CDC_DEBUG=OFF -DENABLE_STANDBY_HID_KEYBOARD=OFF` for the cleanest Bluetooth-only `btusb` check. The default standby-HID build is a composite device and should show both a Bluetooth interface and a boot-keyboard HID interface.

```bash
lsusb -v
dmesg -w
```

Expected:

```text
bInterfaceClass    0xe0 Wireless Controller
bInterfaceSubClass 0x01 RF Controller
bInterfaceProtocol 0x01 Bluetooth
default build also has a HID keyboard interface
btusb attempts to bind
```

## ESP32 UART Backend

The Pico + ESP32 UART-HCI path is secondary hardware now. See [ESP32 HCI UART Backend](esp32_hci_uart.md) for wiring and build commands.
