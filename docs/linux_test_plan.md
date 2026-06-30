# Linux Test Plan

## Descriptor Bring-Up

Build with the CYW43 or stub backend and flash the UF2. For descriptor testing, keep the power LED/sense pin high or disable it with `-DPIN_PWR_OK_SENSE=-1`; otherwise the firmware intentionally disconnects USB in standby wake mode.

Use `ENABLE_CDC_DEBUG=OFF` for the cleanest `btusb` check.

```bash
lsusb -v
dmesg -w
```

Expected:

```text
bInterfaceClass    0xe0 Wireless Controller
bInterfaceSubClass 0x01 RF Controller
bInterfaceProtocol 0x01 Bluetooth
btusb attempts to bind
```

## ESP32 UART Backend

The Pico + ESP32 UART-HCI path is secondary hardware now. See [ESP32 HCI UART Backend](esp32_hci_uart.md) for wiring and build commands.
