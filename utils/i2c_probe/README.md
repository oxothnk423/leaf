# Leaf I2C failure bench tools

Temporary, standalone USB diagnostic firmware for Leaf v3.2.6/v3.2.7. It replaces the Leaf
application while installed; it does not link the normal UI, radio, sensor processing, settings
or storage services. It holds GPIO48 HIGH for the power latch and uses GPIO47 for the LED.
Use USB power for these bench tests. Restore the appropriate normal firmware afterward.

## Firmware targets

| PlatformIO environment | Source | Behavior |
| --- | --- | --- |
| `leaf_gpio_inputs` (default) | `src/gpio_inputs.cpp` | GPIO8, 9, 21, 41, 42 and 45 are inputs with weak pull-ups. The LED is on while any input is LOW, independently of USB logging. |
| `leaf_i2c_probe` | `src/main.cpp` | Input monitor on SDA=GPIO8, SCL=GPIO9. Active electrical tests and address scans run only on command. |
| `leaf_i2c_jumpers` | `src/main.cpp` | Same scanner, but SDA=GPIO42, SCL=GPIO41. Original GPIO8/9 remain inputs without pulls; external routing jumpers are required. |

The six-input test logs state changes and one-second snapshots, including per-pin falling-edge
counters. GPIO21 connects to the backlight circuit and read LOW on the tested board, so its LED
stayed on even when all other pins were HIGH. Verify individual transitions in the serial log.

Scanner commands at 115200 baud:

- `i`: inputs with weak pull-ups; `p`: inputs with weak pull-downs; `f`: floating inputs.
- `w`: toggle one-second input reporting; `?`: help.
- `r`: run passive bias tests, open-drain bus clear/pull-release tests, address-only hardware
  scans at 10/100/400 kHz, and an independent software scan near 10 kHz when the lines permit it.
  The scanner returns to input mode afterward. No sensor register data is written by these scans.

For grounding tests, use input-only mode and connect the selected line to board GND through
the test resistor. The recorded positive control used 100 ohms. Remove every grounding resistor
before sending `r`; keep the SDA/SCL routing jumpers installed for `leaf_i2c_jumpers`. Active
tests use open-drain outputs, never actively drive HIGH, and cannot establish trace continuity
beyond the point read by the processor. Other bus devices can interpret address probes.

## Accessible v3.2.7 expansion connector points

| Signal | H1 pin |
| --- | --- |
| SDA / GPIO8 | 22 |
| SCL / GPIO9 | 21 |
| GPIO21 | 9 |
| GPIO41 | 20 |
| GPIO42 | 18 |
| GPIO45 | 8 |
| GND | 24 |

Source: [v3.2.7 schematic](../../vario/hardware/Schematic_Leaf-esp32-v3.2.7_2025-07-07.pdf).
Do not assume an identically numbered connector on another hardware revision has this mapping.

## Build and flash

From the repository root, using the local PlatformIO installation:

```powershell
$probePython = "$env:USERPROFILE\.platformio\penv\Scripts\python.exe"
$probePio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
$probeEsptool = "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py"
$probeTarget = 'leaf_gpio_inputs' # or leaf_i2c_probe / leaf_i2c_jumpers
& $probePio run -d utils\i2c_probe -e $probeTarget
if ($LASTEXITCODE -ne 0) { throw 'Diagnostic build failed' }
& $probePython -m serial.tools.list_ports -v
$probePort = & $probePython -c "from serial.tools import list_ports; p=[p.device for p in list_ports.comports() if p.vid==0x303A]; assert len(p)==1, 'Expected exactly one Espressif USB port'; print(p[0])"
if ($LASTEXITCODE -ne 0) { throw 'Leaf port is absent or ambiguous' }
$probeImage = "utils\i2c_probe\.pio\build\$probeTarget\firmware.bin"
& $probePython $probeEsptool --chip esp32s3 --port $probePort --baud 460800 --before default_reset --after hard_reset write_flash -z --flash_mode qio --flash_freq 80m --flash_size detect 0x10000 $probeImage 0x340000 $probeImage
if ($LASTEXITCODE -ne 0) { throw 'Diagnostic flash failed' }
& $probePio device monitor --port $probePort --baud 115200
```

These app offsets assume the existing Leaf 8 MB partition layout, with OTA app slots at
`0x10000` and `0x340000`. Do not use them on a different partition layout. Both slots are written
so the selected boot slot does not matter; no bootloader, partition table or settings are erased.
Close any existing serial monitor before flashing. Detect the port again if USB re-enumerates.
Exit the monitor with Ctrl+C. If the scanner reports no input banner, check the USB connection
before sending an active test command. Restore a hardware-matched Leaf image to both slots when
finished; instructor firmware is still proposed and these tools are not an instructor receiver.

## Recorded evidence

The [2026-10-24 v3.2.7 captures](captures/2026-10-24-3.2.7/) retain device identity, firmware
hashes and raw readings. They came from successive diagnostic iterations; their hashes identify
the actual images used and do not imply every capture used the final source in this directory.
Capture timestamps in UTC can fall on October 25; the local bench date was October 24.
Serial line endings and trailing whitespace are normalized for Git; readings are preserved.

- [Original GPIO8/9 scanner](captures/2026-10-24-3.2.7/capture-3.2.7.txt): no device ACKs;
  readings followed internal bias rather than the physical bus pull-ups.
- [Input-only GPIO8/9 watch](captures/2026-10-24-3.2.7/input-watch-3.2.7.txt): no LOW readings
  while the user reported 0.119 V on each grounded physical I2C line.
- [Six-input positive control](captures/2026-10-24-3.2.7/six-input-watch-3.2.7.txt): GPIO41
  registered grounding/release; GPIO8/9 did not. GPIO21 remained LOW.
- [SDA42/SCL41 jumper scanner](captures/2026-10-24-3.2.7/jumper-scan-3.2.7.txt): two runs
  consistently found `0x20`, `0x38`, `0x77` at all three hardware rates and by software scan.
  No IMU responded. Address matches are not full device identity or measurement verification.

This supports a fault in the original GPIO8/9 connections or processor inputs, without locating
the physical break or distinguishing it from damaged pins. The normal firmware fallback and
its simulator regression test are separate from these tools: a working barometer is still
required for the barometer-only vario. See the
[training-link backlog](../../docs/dev-references/backlog/student-instructor-training-link.md)
for the planned sensor-free instructor build and its non-sensor I2C dependencies.
