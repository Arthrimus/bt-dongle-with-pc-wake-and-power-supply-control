# ESP32 HCI UART Backend

This is the secondary hardware path for the original Pico + ESP32 build. Use it when the Pico is connected to an ESP32 running an HCI UART controller firmware such as Espressif's `controller_hci_uart_esp32`.

```text
PC USB host -> Pico TinyUSB Bluetooth HCI device -> UART H4 -> ESP32 controller_hci_uart_esp32
```

The Pico 2 W onboard CYW43 backend is the preferred path for this repo's current development.

## HCI UART Pins

The HCI pins match the original hardware defaults:

| Signal | Pico GPIO |
| --- | ---: |
| Pico UART TX -> ESP32 UART RX | 4 |
| Pico UART RX <- ESP32 UART TX | 5 |
| Pico CTS <- ESP32 RTS | 6 |
| Pico RTS -> ESP32 CTS | 7 |

UART defaults to `uart1`, `921600`, `8N1`, with RTS/CTS hardware flow control enabled.

Optional reset wiring:

| Signal | Pico GPIO |
| --- | ---: |
| ESP32 EN/reset | `PIN_ESP32_RESET`, default `-1` |

## Existing SPI Wiring

The old SPI audio-offload pins may remain physically attached:

```text
GPIO16 MISO
GPIO17 CS
GPIO18 SCK
GPIO19 MOSI
GPIO20 READY
```

This firmware does not initialize SPI, so those pins are not driven as SPI.

## Build

```powershell
cd C:\Users\zheku\pico-usb-bt-wake
cmake -S . -B build-esp32 -G Ninja -DHCI_BACKEND=esp32_uart -DPICO_BOARD=pico2
cmake --build build-esp32
```

Useful ESP32-specific options:

- `-DENABLE_UART_FLOW_CONTROL=ON`
- `-DESP32_HCI_UART_ID=1`
- `-DESP32_HCI_UART_BAUD=921600`
- `-DPIN_ESP32_UART_TX=4`
- `-DPIN_ESP32_UART_RX=5`
- `-DPIN_ESP32_UART_CTS=6`
- `-DPIN_ESP32_UART_RTS=7`
- `-DPIN_ESP32_RESET=<gpio>`

## Bring-Up Notes

Verify the ESP32 controller firmware directly with a USB-UART adapter and `btattach` before inserting the Pico bridge. The common failures are swapped TX/RX, baud mismatch, flow control enabled in firmware but not wired, or ESP32 firmware that is not actually in controller-only HCI UART mode.
