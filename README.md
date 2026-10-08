# Wireless Water Tank Level Monitor

This project measures the water level from the top of a tank and sends the measurement to a separate indicator unit over a UART-connected LoRa link. Both units use a Raspberry Pi Pico (RP2040).

The transmitter uses an AJ-SR04M sensor. Its trigger and echo timing is handled by an RP2040 PIO state machine. The receiver shows the level on an ILI9341 TFT and sounds a buzzer when the transmitter reports a full tank. A push button silences the current alert.

The firmware is written in C. Most peripheral setup and access is done by writing RP2040 memory-mapped registers directly. The Pico SDK is still used for the startup and CMake build system, and for generating the C header from the PIO source.

The transmitter and receiver PCBs were designed in Altium Designer as single-sided boards. The transmitter is powered from a rechargeable 18650 Li-ion cell. The exported schematics and PCB drawings are in the Hardware folder; editable Altium project files and fabrication Gerbers are not included.

## How the data moves

Sensor echo pulse -> transmitter PIO counter -> distance in centimetres -> UART1 -> LoRa radio link -> receiver UART1 -> frame parser and checksum -> RP2040 inter-core FIFO -> display gauge and alert logic.

The transmitter measures on a repeating cycle. The receiver considers a correctly framed packet to mean that the transmitter link is alive, including a packet whose status says the sensor did not get a measurement.

## Hardware and connections

Use two Pico boards, one at the tank and one at the indicator. The transmitter schematic identifies an AJ-SR04M ultrasonic sensor and an EBYTE LORA_E32433T20D UART LoRa module. It also shows an LM2596 regulator module and a logic shifter on the sensor signal path. The receiver schematic shows a matching LoRa module, an ILI9341 display, an LM2596 module, a push button, and a BC547C transistor driving the buzzer. Check the PDFs and your actual module revisions before assembling; module breakout pinouts can vary.

The code exposes the following RP2040 GPIO assignments. These are GPIO numbers, not physical header pin numbers.

Transmitter:

| GPIO | Connection | Notes |
| --- | --- | --- |
| 2 | Sensor TRIG | PIO output; generates the trigger pulse. |
| 3 | Sensor ECHO | PIO input; sampled for echo timing. Check the sensor output voltage is safe for the Pico. |
| 4 | Radio UART1 TX | Pico to radio. |
| 5 | Radio UART1 RX | Radio to Pico. |
| 6 | Radio M0 | Mode select. |
| 7 | Radio M1 | Mode select. |
| 8 | Radio AUX | Module status input. |
| 0 | UART0 TX | Debug output. |
| 25 | On-board LED | Configured as output. |

Receiver:

| GPIO | Connection | Notes |
| --- | --- | --- |
| 4 | Radio UART1 TX | Pico to radio. |
| 5 | Radio UART1 RX | Radio to Pico. |
| 6 | Radio M0 | Mode select. |
| 7 | Radio M1 | Mode select. |
| 8 | Radio AUX | Module status input. |
| 9 | Buzzer driver | Active-high input to a BC547 low-side switch. Do not connect a buzzer load directly to the GPIO. |
| 10 | Mute button | Active-low input with internal pull-up and 30 ms debounce. |
| 16 | SPI0 RX / MISO | Display SPI assignment. |
| 17 | SPI0 chip select | Display. |
| 18 | SPI0 SCK | Display clock. |
| 19 | SPI0 TX / MOSI | Display data. |
| 20 | Display C/D | Command or data select. |
| 21 | Display reset | Hardware reset. |
| 25 | On-board LED | Receiver heartbeat, controlled by Core 1. |
| 0 | UART0 TX | Debug output. |

GPIO4 and GPIO5 use the RP2040 UART1 alternate function. The display uses SPI0. Check the pinout and voltage levels of the exact radio, display, and sensor boards against the schematic and PCB drawing before applying power.

The transmitter uses a protected 18650 Li-ion cell and a suitable power path for the Pico, sensor, and radio. The firmware does not measure battery voltage or control charging. Do not connect a bare cell directly to a supply rail unless that rail and the required protection/regulation are designed for it.

## Transmitter measurement: GPIO timing to PIO

The first version measured the echo pulse by polling a normal GPIO and reading a timer. That approach made the CPU responsible for noticing both edges. The current version moves the time-critical edge handling into PIO, which can execute its own small instruction program while the main core does other work.

The PIO program is in Indicator_TX_PIO_Side/ajsr04t.pio. CMake converts it to a generated header during configuration/build. The program configures GPIO2 as the trigger output and GPIO3 as its jump input. For each measurement it:

1. Receives a 40,000-count timeout budget through the PIO TX FIFO.
2. Waits for the echo pin to be low before starting. This avoids triggering while the sensor line is still active.
3. Raises the trigger output for 10 microseconds, then lowers it.
4. Waits for the echo rising edge, bounded by the timeout budget.
5. Reloads the counter and counts down while the echo pin stays high.
6. Pushes the remaining count to the PIO RX FIFO when the echo falls or the wait times out.

The state machine clock is divided to 2 MHz. The echo-count loop takes two PIO cycles per decrement, so each decrement represents approximately one microsecond. The C helper calculates duration as 40000 - remaining. A zero count is treated as no echo. For a valid duration, the transmitter estimates distance with duration_us / 58 to get centimetres.

The transmitter sends a measurement about every 600 ms: it waits 100 ms after sending for the radio module and then waits another 500 ms before the next measurement.

## Radio data format

UART1 is configured for 9600 baud, 8 data bits, no parity, and one stop bit (8-N-1). UART0 is used for debug prints. The transmitter writes nine bytes for each message:

| Byte | Value | Purpose |
| --- | --- | --- |
| 0 | 0x00 | Leading radio/address byte. |
| 1 | 0x02 | Leading radio/address byte. |
| 2 | 0x17 | Leading radio/channel byte. |
| 3 | 0xAA | Application frame start. |
| 4 | Status | 1 = valid distance, 2 = full condition, 3 = no measurement. |
| 5 | Distance high byte | High byte of centimetre distance; zero for status 2 or 3. |
| 6 | Distance low byte | Low byte of centimetre distance; zero for status 2 or 3. |
| 7 | Checksum | XOR of status, distance high byte, and distance low byte. |
| 8 | 0x55 | Application frame end. |

The receiver scans the UART byte stream until it sees 0xAA. It then reads the next five bytes and accepts the frame only when the XOR checksum matches and the last byte is 0x55. The three leading bytes are outside this six-byte application frame. They depend on the radio module's address/channel configuration; make sure both radio modules are configured compatibly. The code includes helpers for writing and reading the radio configuration using M0/M1 mode pins and UART.

In status 1, distance is sent big-endian: high byte first, then low byte. The receiver calculates the level using 20 cm as full and 150 cm as empty:

percent_full = clamp((150 - distance_cm) * 100 / (150 - 20), 0, 100)

Status 2 is sent when the transmitter sees a distance at or below 20 cm, or a distance above 500 cm. The receiver interprets status 2 as full and displays 100%. Confirm that the above-500-cm behavior is suitable for the actual tank installation; it is part of the current firmware's classification logic.

Status 3 means the transmitter could not obtain a measurement. The transmitter still sends a frame with status 3, zero distance bytes, a checksum, and the 0x55 terminator.

## Receiver: radio parsing and dual-core work

The receiver started as a single-core program that had to receive radio data, update the display, and handle the alert. The current version separates these jobs across the RP2040's two Cortex-M0+ cores.

Core 0 initializes the display, UARTs, and radio pins, then parses UART1. It checks the start byte, checksum, and end byte. For an accepted frame it packs status and distance into a 32-bit value and writes it to the RP2040 SIO inter-core FIFO. Status occupies bits 0 through 7; distance in centimetres occupies bits 8 through 23.

Core 1 reads the FIFO and owns the display and alert state. It updates the gauge for status 1, sets it to 100% for status 2, polls the button, drives the buzzer, and toggles GPIO25 every 500 ms. The gauge is drawn on the ILI9341 over SPI0 by the local display driver; it is only redrawn when the reported percentage changes.

Core 1 is launched by receiver code that prepares a stack and aligned vector table, then performs the RP2040 FIFO startup handshake and sends an event to wake the core. The firmware does not use the SDK multicore launch helper. After startup, the FIFO acts as a single-producer/single-consumer handoff: Core 0 produces validated readings and Core 1 consumes them.

When a full status first arrives, Core 1 starts a 15-second alert timer. GPIO9 goes high to turn on the BC547 buzzer driver while the full alert is active. Pressing the active-low button on GPIO10 silences the alert; the input must remain stable for 30 ms to count as a press. If it is not pressed, the buzzer is automatically silenced after 15 seconds. A later non-full status clears the full condition and allows a new full alert.

The transmitter-offline screen is controlled separately from the sensor status. Core 1 stores the time it last received any valid frame and draws the offline screen if that time is more than 15 seconds old. This means status 3 still proves that frames are arriving and resets the offline timer, even though it does not update the gauge. When a new valid frame arrives after an offline screen, the receiver clears the offline state and redraws the gauge.

## Bare-metal details

The application configures peripheral registers directly through volatile pointers to their memory-mapped addresses. The code uses:

- SIO registers for GPIO input/output, output-enable set/clear, and the inter-core FIFO.
- IO_BANK0 and PADS_BANK0 for GPIO function selection, input enable, and pull configuration.
- UART0 and UART1 registers for baud divisors, FIFO status, data, and line format.
- The RP2040 hardware timer for 64-bit microsecond time reads and delay helpers.
- SPI0 registers for the display connection.
- PIO0 registers for instruction memory, pin selection, clock divider, state-machine control, and FIFOs.

The timer helper reads the upper counter, lower counter, and upper counter again. It retries if the upper value changed during the read, preventing a torn 64-bit timestamp at rollover. delay_ms and delay_us are busy waits based on elapsed timer counts.

The display driver initializes SPI0 and the ILI9341 with register writes, then provides small drawing helpers for pixels/areas, characters, text, the tank outline, the fill gauge, and the offline screen. There is no display framework or graphics library in this code.

This is bare-metal-style application code, but it is not a fully SDK-free project. The Pico SDK supplies the project startup/build environment, and the transmitter CMake file uses pico_generate_pio_header to assemble the PIO source into a C header. Peripheral behavior described above is configured in the project code rather than through high-level SDK GPIO/UART/SPI/multicore calls.

## Build and flash

Install the Raspberry Pi Pico SDK 2.3.1, CMake 3.21 or newer, Ninja, and the ARM GNU Embedded toolchain. The checked-in presets expect the SDK under $USERPROFILE/.pico-sdk/sdk/2.3.1 and Ninja under $USERPROFILE/.pico-sdk/ninja/v1.13.2 on Windows. If these are installed elsewhere, update the environment entries in that project's CMakePresets.json or set PICO_SDK_PATH and make Ninja available on PATH.

Build the transmitter from PowerShell by running cd Indicator_TX_PIO_Side, cmake --preset default, and cmake --build --preset default. Build the receiver in the same way from Indicator_RX_DualCore. Each preset writes its output to that project's build directory. The Pico SDK generates UF2 output alongside the ELF, BIN, and HEX files.

To flash a board, hold BOOTSEL while connecting that Pico to USB. When the RPI-RP2 drive appears, copy the matching project's .uf2 file from its build directory to the drive. Flash the transmitter image to the tank-side board and the receiver image to the indicator board.

Each folder is a separate CMake project and has its own CMake preset and VS Code configuration. Open one project folder at a time in VS Code so the CMake extension selects the correct source directory and preset. The VS Code settings include local Pico SDK paths and may need to be adjusted on another machine.

## Hardware design files

The Hardware folder contains the following four PDF exports from the Altium designs:

- [Transmitter schematic](Hardware/TX_Side_Schematic.pdf) shows the Pico, AJ-SR04M sensor, LORA_E32433T20D radio, LM2596 module, and logic shifter connections.
- [Transmitter PCB drawing](Hardware/TX_Side_PCB.pdf) is the single-sided transmitter board layout.
- [Receiver schematic](Hardware/RX_Side_Schematic.pdf) shows the Pico, ILI9341 display, LORA_E32433T20D radio, LM2596 module, button, and BC547C buzzer driver connections.
- [Receiver PCB drawing](Hardware/RX_Side_PCB.pdf) is the single-sided receiver board layout.

Use the schematic PDFs to follow the circuit connections and the PCB PDFs to review the board layouts. These are PDF exports for reference; they are not editable Altium source files or manufacturing Gerbers. The transmitter schematic shows a battery input and an LM2596 module, but the PDF does not document the 18650 cell protection or charging circuit. Verify the power path and component ratings against the actual hardware before building.

## Repository files

Indicator_TX_PIO_Side contains the transmitter main program, the PIO program, its setup/result helpers, UART and timer drivers, LoRa control helpers, and its CMake files.

Indicator_RX_DualCore contains the receiver main program, the ILI9341 driver and font data, UART and timer drivers, LoRa control helpers, and its CMake files.

The root .gitignore excludes build output and generated firmware files while leaving the project .vscode directories available for version control.

## Calibration and things to check on a new build

- Set the receiver's FULL_CM and EMPTY_CM values to match the installed sensor position and tank. The current values are 20 cm and 150 cm.
- Check the transmitter's full/near threshold and the greater-than-500-cm classification against the tank dimensions and the sensor's usable range.
- Verify sensor trigger/echo voltage levels, radio UART wiring, M0/M1/AUX wiring, display SPI wiring, and the buzzer transistor circuit against the actual boards.
- Confirm both radio modules use the same compatible settings for UART baud, channel, address, air data rate, and operating mode. The exact radio model is not recorded in the source, so use that module's manual for configuration values.
- The receiver's offline timeout is 15 seconds. Increase it if the transmit interval or radio duty cycle changes substantially.
- The firmware has no battery monitor or charger control. Check the cell protection and voltage regulation in the hardware design.




