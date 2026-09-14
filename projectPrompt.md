# Project Prompt

## Non-negotiable requirements
Use native ESP-IDF (not Arduino) and keep the project directly usable with the Espressif VS Code extension.

## Source of Truth

The existing source code is the only authoritative specification for this project.

When documentation, comments, generated files, assumptions, or external examples disagree with the source code, follow the source code. Do not silently change working behavior to match documentation. Update documentation only when the source implementation has intentionally changed.

Preserve existing functionality and architecture unless a requested change requires otherwise. Prefer the smallest focused change that fits the existing implementation.

## Project Identity

This is a native ESP-IDF C application for an ESP32 target.

The hardware target is:

- M5Stack Core Basic v2.7
- ESP32
- 320x240 ILI9342C display
- M5Stack GPS Module v2.0 using an AT6668 GNSS receiver

Do not introduce Arduino, M5Unified, TinyGPS++, or other external frameworks or components unless explicitly requested.

## Language And Formatting

- Use C for application and component code.
- Use Allman brace style.
- Use two spaces for indentation.
- Use lowerCamelCase for functions and variables where the surrounding public API permits it.
- Use descriptive names; do not use one-letter variable names except for tightly scoped conventional indices.
- Preserve existing whitespace and formatting in unrelated code.
- Do not perform broad reformatting.
- Use standard C types and ESP-IDF types consistently with the existing code.
- Prefer `std::string` only in C++ code; this project is C and should use C strings and fixed buffers as already implemented.

## Comment Rules

Comments must never appear beside code on the same line.

A comment must always appear above the code line, code block, or function it describes.

Never use block comments. Do not use `/* ... */` comments anywhere in new or modified code.

Every comment must begin with the exact prefix `//-- `.

Example:

```c
//-- Configure the display pins before initializing the SPI bus.
configure_display_pins();
```

Keep comments concise and useful. Do not add comments that merely restate obvious code. Existing comments are part of the current source reference, but any new or modified comments must follow these rules.

## ESP-IDF Rules

- Use ESP-IDF APIs, component registration, FreeRTOS, and ESP-IDF types.
- Keep ESP-IDF component dependencies explicit in each component's `CMakeLists.txt`.
- Use the split ESP-IDF driver components where required by the installed ESP-IDF version.
- The board component currently uses the legacy I2C API through the `driver` component and also requires the split GPIO and I2C components.
- The GPS component uses `esp_driver_uart`.
- The LCD component uses `esp_driver_gpio` and `esp_driver_spi`.
- Do not mix Arduino APIs into this project.
- Do not invent ESP-IDF APIs, configuration options, or component names.
- Validate changes with an ESP-IDF build.
- Never flash or upload firmware automatically. The user performs flashing manually.
- idf.py is located in `$HOME/.espressif/tools/activate_idf_v6.0.2.sh`

## Hardware Pinout

### LCD

The native LCD driver is in `components/lcd/lcd.c` and uses SPI3:

- MOSI: GPIO23
- SCLK: GPIO18
- CS: GPIO14
- DC: GPIO27
- RESET: GPIO33
- Backlight: GPIO32
- SPI mode: 0
- SPI clock: 40 MHz
- Display size: 320x240
- Pixel format: RGB565

The current display orientation is controlled by the ILI9342C MADCTL command. The working value in the source is `0x08`. Preserve this value unless the physical display orientation is deliberately revalidated on the actual M5Stack hardware.

The display has the buttons physically below it. Preserve the current top-to-bottom screen geometry and readable text orientation.

The LCD is driven directly through SPI. Do not replace it with a graphics framework or a different display abstraction without an explicit request.

The LCD and SD card share the M5Stack VSPI bus on SPI3. The shared bus pins are MOSI GPIO23, MISO GPIO19, and SCLK GPIO18. LCD CS is GPIO14 and SD CS is GPIO4. Do not initialize a second SPI bus for the SD card or change either device's chip-select pin.

### LCD Color Correctness (confirmed on hardware)

This specific ILI9342C panel requires Display Inversion mode to be explicitly turned **ON** during init, or every color renders as its bitwise-inverted opposite (for example intended white renders as black, intended green renders as purple). This was diagnosed and confirmed using `lcd_color_test()` in `components/lcd/lcd.c`, which draws labeled RGB565 bars on screen for a physical photo comparison.

Required and confirmed-working fix:

- `lcd_init()` in `components/lcd/lcd.c` must send `cmd(0x21)` (Display Inversion ON) during panel initialization, after the gamma/COLMOD setup and before sleep-out (`cmd(0x11)`). Do not remove this command or change it to `0x20`.
- With `cmd(0x21)` sent, `LCD_COLOR_*` macros in `components/lcd/include/lcd.h` use **true, standard RGB565 values** (no byte-swapping or bit-inversion pre-correction needed).

Confirmed primary/secondary color macros (standard RGB565, verified correct on the physical display with `cmd(0x21)` active):

```c
#define LCD_COLOR_BLACK    0x0000
#define LCD_COLOR_WHITE    0xFFFF
#define LCD_COLOR_RED      0xF800
#define LCD_COLOR_GREEN    0x07E0
#define LCD_COLOR_BLUE     0x001F
#define LCD_COLOR_YELLOW   0xFFE0
#define LCD_COLOR_CYAN     0x07FF
#define LCD_COLOR_MAGENTA  0xF81F
```

Rules for future color changes:

- Always add new UI colors as a named `LCD_COLOR_*` macro in `lcd.h` using a true, standard RGB565 value. Never use a raw hex color directly in draw calls.
- Never remove or bypass `cmd(0x21)` in `lcd_init()`. If colors ever look wrong again on this panel, verify Display Inversion state first before assuming a color macro or byte-order problem.
- If a different physical display panel is ever substituted, re-run the `lcd_color_test()` bar test and re-confirm before trusting these values.

### Buttons And Battery

The board component uses:

- Button A: GPIO39
- Button B: GPIO38
- Button C: GPIO37
- I2C bus: I2C0
- I2C SDA: GPIO21
- I2C SCL: GPIO22
- IP5306 address: `0x75`

Buttons are active low. Button events are debounced and long presses are recognized by the board task. A long press must execute immediately once the long-press threshold is reached; it must not wait for the button to be released. Short presses remain valid after the debounce threshold and are only generated on release.

### GPS

The GPS configuration is defined in `main/app_main.c` and must remain consistent with the hardware:

- UART: UART2
- GPS RX into ESP32: GPIO16
- GPS TX from ESP32: GPIO17
- Baud rate: 115200
- Frame: 8N1
- Hardware flow control: disabled
- Position update request: 10 Hz through `$PCAS02,100*1E`

The GPS parser accepts checksummed NMEA RMC and GGA sentences. Preserve checksum validation, fix handling, satellite count handling, and the thread-safe latest-data interface.

## SD Card GPS Export

All valid GPS coordinates must be written to the SD card for the active trip.

Each trip must use a new Google Maps-compatible KML file when the trip is reset. File names must use this exact form:

```text
trip-EEYYMMDD-HH:mm.kml
```

- The numeric identifier `EEYY` is the Centery+Year (2025, 2026 etc.).
- The numeric identifier `MM` is the Month (01 .. 12)
- The numeric identifier `DD` is the current Day (01 .. 31).
- The numeric identifier `HH` is the current Hour (01 .. 24).
- The numeric identifier `mm` is the current Minute (00 ..59).

If the SDcard has less then 20% free space remove the oldest files until there is again more then 20% free space.

KML must contain valid GPS coordinate data in the format expected by Google Maps or Google My Maps. Preserve coordinate order as longitude, latitude, altitude where altitude is available. Write only valid GPS fixes and handle file-open, write, sync, close, and card errors explicitly.

Trip reset must close the current export file before creating the next file. The new file must be initialized with valid KML structure before coordinates are appended, and it must be finalized correctly when the trip ends or the application shuts down where the platform allows it.

The implementation must use the project's actual SD-card hardware and ESP-IDF support. Do not invent SD-card pins, mount points, host settings, or APIs. Add the required component dependencies explicitly and keep SD-card ownership in a dedicated component unless the existing architecture provides a better local owner.

## Component Responsibilities

### `main`

`main/app_main.c` owns application orchestration:

- Initialize NVS, board, LCD, GPS, and speedometer services.
- Handle button events.
- Select current speed versus trip average in SPEED mode and in the lower part of TRIP mode.
- Select TRIP mode versus SPEED mode for the main display.
- Provide the current trip distance in the lower part of SPEED mode.
- Manage display backlight timeout.
- Persist TOTAL distance through NVS.
- Refresh the LCD at the existing cadence.

Do not move component responsibilities into `app_main.c` unless necessary.

### `components/board`

Owns button input, button event generation, I2C initialization, and IP5306 battery and charging status.

### `components/gps`

Owns UART configuration, GNSS command transmission, NMEA parsing, checksum validation, and synchronized latest GPS data.

### `components/lcd`

Owns the native ILI9342C SPI protocol, display initialization, primitives, custom glyphs, seven-segment speed digits, screen layout, colors, and rendering state.

The display uses a custom small bitmap glyph renderer. Do not replace it with a font library unless explicitly requested.

Current UI dimensions and layout are source-defined and must be treated as authoritative. The large speed readout uses custom seven-segment digits. Smaller labels use the existing bitmap glyph renderer and current scale values.

### `components/speedometer`

Owns GNSS speed filtering, display speed limiting, trip distance, total distance, trip reset, and trip average calculations.

Distance is integrated from valid GNSS speed. Preserve the current stationary threshold, filtering behavior, timing safeguards, and range limits.

### SD-card storage

The SD-card storage implementation owns card mounting, free-space reporting, trip file numbering, KML file lifecycle, coordinate appends, flushing, and error handling. GPS parsing and display rendering must not directly own SD-card protocol details.

Free-space reporting must use the mounted FatFs volume and `f_getfree()`. Do not use `statvfs()` for SD-card capacity reporting because the selected ESP-IDF version does not implement it and returns `ENOSYS`. A mounted card must remain usable even when a filesystem-statistics read fails; report the error without crashing.

Formatting must use the ESP-IDF SD-card FAT formatter. The current trip file must be closed before formatting, and a new `trip-nnn.kml` file must be created after formatting succeeds.

## User Interface Behavior

The current controls are source-defined:

- Short Button A activates TRIP mode.
- Long Button A resets the active trip and creates the next `trip-YYMMDD-HHmm.kml` export file using the current date-time stamp.
- Short Button B toggles the display backlight on or off when the system menu is closed.
- Long Button B opens or closes the system menu.
- Short Button C activates SPEED mode and toggles between SPEED and AVG SPEED.
- Pressing a button while the display is off wakes it.

Every button press/release is logged with the physical position, button name, `SHORT` or `LONG` press classification, and press duration. Menu cursor changes and selected actions are also logged.


### System Menu

Opening the [System Menu] must not start WiFi or the webserver. WiFi and the webserver are only started from the [WiFi Menu].

When the [WiFi Menu] is entered, that menu stays active until a LONG-press on Button B is detected. It does not close on a short-button action or on key release.

While the system menu is open, the normal application functions of all buttons are disabled:

- Short Button A moves the purple cursor up.
- Short Button C moves the purple cursor down.
- Short Button B executes the function under the cursor.
- Long Button B closes the system menu.

The menu options are:

1. `New Trip File`: resets the active trip and creates a new KML file named with the current date-time in the `trip-YYMMDD-HHmm.kml` format.
2. `Show Used/Free`: shows the actual used and free SD-card space in kB on the Action screen.
3. Enter `[WiFi Menu]`
4. Reset Tracker (esp32.restart)
5. `Format SD`: formats the SD card, creates a new trip file, and returns to the system menu after formatting succeeds.
6. `Exit`: closes the system menu.

After a short Button B execution, the display is cleared and shows an Action screen. Reset and format actions show `EXECUTING` and the selected operation. The `Show Used/Free` Action screen shows SD-card information instead of the execution text. Action screens remain visible until another short Button B press returns to the system menu, except that a successful `Format SD` action returns automatically to the system menu.

Every button release is logged with the physical position, button name, `SHORT` or `LONG` press classification, and press duration. Menu cursor changes and selected actions are also logged.

#### In TRIP mode:

- Show the trip distance as the large central value using the same large seven-segment digit style as the speed value.
- Show `SPEED` or `AVG SPEED` at the left of the lower display area, followed by the current or average speed value.
- Keep the SD-card free-space indicator in the lower display area.

#### In SPEED mode:

- Show current speed or average speed as the large central value using large seven-segment digits.
- Show `TRIP` at the left of the lower display area, followed by the trip distance value.
- Keep the SD-card free-space indicator in the lower display area.

The main-mode labels `TRIP`, `SPEED`, and `AVG SPEED` are rendered horizontally. In the lower display area, the label must be placed to the left of its value. The `AVG SPEED` label must not overlap the speed value; the value must move to the right when the wider label is shown.

The display also shows:

- GPS fix state.
- Satellite count.
- Battery level.
- Charging state.

The lower display area must include a storage indicator bar showing the remaining usable SD-card space. Used space must be black and free space must be green. The bar must update from actual mounted-card FatFs capacity and free-space values, not from a hard-coded estimate. Handle an absent, unmounted, or unreadable card with a clear red error state without crashing the application. SD-card error text must be rendered in red consistently.

The main display layout uses a 320x240 screen. The separator below the middle section is rendered below the large seven-segment digits, and the large digits are positioned low enough that the `TRIP`, `SPEED`, and `AVG SPEED` headings remain readable without overlap.

The backlight timeout depends on battery level and is disabled while charging. Preserve the existing timeout behavior in `app_main.c`.

TOTAL distance is stored in NVS under the existing namespace and key. Preserve the current checkpoint strategy and units.

### WiFi Menu

The top part of the screen shows "WiFi Menu".

The [WiFi Menu] must always show a clear state/status message describing what it is doing. The text "EXECUTING WIFI MENU" is not valid and must not be used. Use explicit status names such as:

- `CONNECTING TO AP`
- `CAPTIVE PORTAL ACTIVE`
- `CONNECTED TO AP`
- `BROWSE TO <hostName>.local`
- `CLIENT ACCESS <IP-address>`
- `WIFI MENU CLOSED`

WiFi and the webserver are only active while the [WiFi Menu] is open. In the normal application display or in the [System Menu], the webserver must be stopped and WiFi must be off to minimize power usage. The [WiFi Menu] must not close itself automatically because that would drop the WiFi connection. It remains active until a LONG-press on Button B is detected.

While the [WiFi Menu] is open, the normal application functions of all buttons are disabled except for:

- Long Button B closes the [WiFi Menu] and returns to [System Menu].

Entering the menu activates:

1 - Print "Connecting to AP", Start WiFi with known credentials. If after trying it is not possible to connect to the AP print "`Starting Captive Portal`", "`Select WiFi network <hostName> and browse to 192.168.1.4`", Start the Captive Portal.

2 - If connecting to the AP succeeds print "`Connected to <SSID> with <IP-ADDRESS>`"

3 - Start webserver (GUI) and print "`Browse to <hostName>.local`"

4 - If a client connect to the webserver (GUI) print "Accessed by `<IP-address client>`"

The [WiFi Menu] remains active until a LONG-press on Button B is detected; a short press does not close it. Long-press actions must be executed immediately once the threshold is reached, not after key release.

Trip filenames must use the GPS date/time when a valid GPS date is available, because that is the source of truth for the trip export. If a valid GPS date is not available yet, log a warning and continue using the best available fallback timestamp without aborting the trip file creation.

## Validation

After code changes:

1. Build the project with the ESP-IDF VS Code integration or an ESP-IDF terminal.
2. Confirm that the application and bootloader build successfully.
3. Confirm that the application binary fits the configured app partition.
4. Do not run flash, upload, erase, or monitor commands automatically.
5. Report any remaining compiler warnings or validation limitations clearly.

## Change Discipline

Make focused edits. Do not remove existing functionality. Do not change hardware pin assignments, display orientation, protocol settings, persistent-storage keys, or user controls without explicit evidence from the source and a direct task requirement.

## Miscalanious

The idf.py command is in 'source "$HOME/.espressif/tools/activate_idf_v6.0.2.sh"'

