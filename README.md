# SmartFeeder ESP32-C3

Firmware for the SmartFeeder ESP32-C3 prototype.

## Current hardware

| Function | GPIO |
| --- | ---: |
| Shared ultrasonic trigger | 4 |
| Hopper ultrasonic echo | 5 |
| Pet ultrasonic echo | 6 |
| HX711 DOUT | 7 |
| HX711 SCK | 10 |
| Servo signal | 3 |

The servo must use an external 5 V supply with a common ground. Do not power
the servo from the ESP32-C3 3.3 V regulator.

## Firmware behavior

- Opens `SmartFeeder-Setup` when no saved Wi-Fi network is available.
- Access point password: `smartfeeder`.
- Stores Wi-Fi, backend URL, feeder ID, HX711 calibration, and servo position in
  NVS.
- Sends telemetry immediately after connecting and then once per minute.
- Polls backend commands once per minute.
- Sends an on-demand telemetry response when the frontend requests a refresh.
- Confirms pet presence with consecutive valid ultrasonic readings.
- Freezes the plate weight while the pet is present.
- Measures the plate again after confirmed absence and reports `detected` and
  `left` events for consumption calculation.
- Requires two stable weight confirmations before completing a dispense.
- Restores the last servo position at boot without running a dispense test.

## Serial commands

Open the monitor at `115200` baud:

```text
help
test
0
90
180
hx711 status
hx711 tare
hx711 raw
hx711 read
hx711 reset
hx711 calibrate <grams>
```

Calibrate with the plate empty first:

1. Send `hx711 tare` with no food on the plate.
2. Place a known weight on the plate.
3. Send `hx711 calibrate 100` (replace `100` with the known grams).
4. Verify with `hx711 read`.

## Build

```text
pio run
pio run --target upload
pio device monitor
```

The PlatformIO configuration targets `esp32-c3-devkitm-1`.
