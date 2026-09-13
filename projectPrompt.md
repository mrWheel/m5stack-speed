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

### Buttons And Battery

The board component uses:

- Button A: GPIO39
- Button B: GPIO38
- Button C: GPIO37
- I2C bus: I2C0
- I2C SDA: GPIO21
- I2C SCL: GPIO22
- IP5306 address: `0x75`

Buttons are active low. Button events are debounced and long presses are recognized by the board task.

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
trip-nnn.kml
```

The numeric identifier `nnn` must be zero-padded and range from `000` through `999`. Select the next available identifier without overwriting an existing trip file. If all identifiers are occupied, report the storage error and do not silently overwrite a file.

KML must contain valid GPS coordinate data in the format expected by Google Maps or Google My Maps. Preserve coordinate order as longitude, latitude, altitude where altitude is available. Write only valid GPS fixes and handle file-open, write, sync, close, and card errors explicitly.

Trip reset must close the current export file before creating the next file. The new file must be initialized with valid KML structure before coordinates are appended, and it must be finalized correctly when the trip ends or the application shuts down where the platform allows it.

The implementation must use the project's actual SD-card hardware and ESP-IDF support. Do not invent SD-card pins, mount points, host settings, or APIs. Add the required component dependencies explicitly and keep SD-card ownership in a dedicated component unless the existing architecture provides a better local owner.

## Component Responsibilities

### `main`

`main/app_main.c` owns application orchestration:

- Initialize NVS, board, LCD, GPS, and speedometer services.
- Handle button events.
- Select current speed versus trip average.
- Select TRIP versus TOTAL distance.
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

- Short Button A toggles the displayed distance between TRIP and TOTAL.
- Long Button A resets the active trip and creates the next `trip-nnn.kml` export file.
- Short Button B toggles the display backlight on or off when the system menu is closed.
- Long Button B opens or closes the system menu.
- Short Button C toggles SPEED and AVG SPEED when the system menu is closed.
- Pressing a button while the display is off wakes it.

### System Menu

While the system menu is open, the normal application functions of all buttons are disabled:

- Short Button A moves the purple cursor up.
- Short Button C moves the purple cursor down.
- Short Button B executes the function under the cursor.
- Long Button B closes the system menu.

The menu options are:

1. `Reset Trip`: resets the active trip and creates a new KML file.
2. `Used Free`: shows the actual used and free SD-card space in kB on the Action screen.
3. `Format SD`: formats the SD card, creates a new trip file, and returns to the system menu after formatting succeeds.
4. `Exit`: closes the system menu.

After a short Button B execution, the display is cleared and shows an Action screen. Reset and format actions show `EXECUTING` and the selected operation. The `Used Free` Action screen shows SD-card information instead of the execution text. Action screens remain visible until another short Button B press returns to the system menu, except that a successful `Format SD` action returns automatically to the system menu.

Every button release is logged with the physical position, button name, `SHORT` or `LONG` press classification, and press duration. Menu cursor changes and selected actions are also logged.

In TRIP mode:

- Show the trip distance as the large central value.
- Show the active trip indicator and trip information in the lower display area.
- Keep the SD-card free-space indicator in the lower display area.

In SPEED mode:

- Show current speed or average speed as the large central value.
- Show the selected distance value in the lower display area.
- Keep the SD-card free-space indicator in the lower display area.

The labels `SPEED` and `AVG SPEED` must be rendered vertically beside the final digit of the large speed value. They must remain readable and must not be rendered as mirrored text.

The display also shows:

- GPS fix state.
- Satellite count.
- Battery level.
- Charging state.

The lower display area must include a storage indicator bar showing the remaining usable SD-card space. The bar must update from actual mounted-card FatFs capacity and free-space values, not from a hard-coded estimate. Handle an absent, unmounted, or unreadable card with a clear red error state without crashing the application. SD-card error text must be rendered in red consistently.

The backlight timeout depends on battery level and is disabled while charging. Preserve the existing timeout behavior in `app_main.c`.

TOTAL distance is stored in NVS under the existing namespace and key. Preserve the current checkpoint strategy and units.

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

