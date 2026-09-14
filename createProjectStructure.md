# createProjectStructure.py

This document reflects the current implementation in `createProjectStructure.py`. The script itself is the source of truth.

## Purpose

`createProjectStructure.py` prepares a deployable `projects/<project-name>/` tree from a PlatformIO or ESP-IDF project. It can either:

- build a project and collect artifacts for deployment, or
- validate an existing local `projects/` tree and sync it to AWS with `rsync`.

The script version currently reported in source is `v2.7 (2026-08-12)`.

## Supported project types

The script detects the project type before building:

- `platformio.ini` present: treated as PlatformIO
- `CMakeLists.txt` present, with either `project.cmake` in the file or a `sdkconfig` file: treated as ESP-IDF

If neither condition matches, it exits with a runtime error.

## Private configuration

The script reads a private JSON file from:

- `~/.ssh/createProjectStructure.json`

This file is required for any metadata-image generation or AWS sync. Required keys depend on the action:

- `project_image_url` for generation of default project metadata
- `aws_server`, `aws_target`, `aws_ssh_key` for AWS operations

If the file is missing or values are invalid, the script raises a runtime error.

## Version detection

Before building, the script validates the project version by locating a source file that contains `PROG_VERSION`.

It checks in this order:

- `src/main.cpp`
- `src/main.c`
- `main/main.cpp`
- `main/main.c`
- `main/app_main.c`
- `main.cpp`
- `main.c`
- any `*/main.cpp`
- any `*/main.c`

If it cannot find a file with `PROG_VERSION`, it aborts. The accepted version forms are:

- `1.2`
- `v1.2`
- `1.2.3`
- `v1.2.3`

The normalized output is always written as `vX.Y` or `vX.Y.Z`.

## Build-system behavior

### PlatformIO path

For PlatformIO projects, the script:

- verifies that `pio` is available in `PATH`
- reads `platformio.ini`
- parses `[env:...]` sections
- skips any environment whose name contains `skip` (case-insensitive)
- fails if no non-skipped envs remain

It also resolves:

- `workspace_dir` from `[platformio]` in `platformio.ini`
- otherwise falls back to `projectPath/.pio`

It resolves board names and SOC family per environment by reading `board` and `platform`, then normalizing values to detect ESP8266 vs ESP32.

### ESP-IDF path

For ESP-IDF projects, the script:

- calls `idf.py build` directly if `idf.py` and `IDF_PATH` are both available
- otherwise tries to source the ESP-IDF environment via the detected `export.sh`
- handles the cached Python path mismatch by running `idf.py fullclean` and rebuilding when the message matches the ESP-IDF error text

## Metadata default generation and validation

The script creates a `projectMetaData/` directory in the project root if it does not exist.

Generated default files are:

- `project_en.md`
- `project_nl.md`
- `project.json`
- `thisProject.png` downloaded from `project_image_url`, or an empty file if download fails
- `.createProjectStructure-defaults.json` marker file tracking original default hashes

Then it validates metadata with `validateProjectMetaData()`:

- required files are `project_en.md`, `project_nl.md`, `project.json`
- either `thisProject.png` or `ESP32project.png` must exist
- if default placeholder content is still present, it raises a runtime error and tells the user which files must be customized

Finally it copies all metadata files from `projectMetaData/` into the generated project target directory, excluding the marker file and renaming `ESP32project.png` to `thisProject.png` if needed.

## Output structure

The output root is:

- `projects/<project-name>/`

Existing output is removed and recreated before build.

### PlatformIO output layout

For PlatformIO, each environment produces a versioned artifact directory.

- If a board is used by only one env: `projects/<project-name>/<board>/<version>/`
- If the same board appears in multiple envs: `projects/<project-name>/<env>/<board>/<version>/`

Typical files inside a version directory include:

- `firmware.bin` (required)
- `flash.json`
- `build_log.md`
- optional files such as `bootloader.bin`, `partitions.bin`, `boot_app0.bin`, `partitions.csv`, `ldscript.ld`, `LittleFS.bin`, etc.

### ESP-IDF output layout

For ESP-IDF, the target directory is:

- `projects/<project-name>/<board-name>/<version>/`

Where board name is derived from the flashed target metadata (`project_description.json` -> `target`, or `flasher_args.json` -> `extra_esptool_args.chip`, otherwise `esp32`).

## PlatformIO artifact collection

For each non-skipped env, the script runs:

- `pio run -e <env>`

If `data/` exists, it also tries:

- `pio run -e <env> -t buildfs`

Any `buildfs` failure is captured as a warning and logged, but it does not stop the build.

It then resolves the build directory from the workspace:

- `${workspace_dir}/build/<env>`
- else `${workspace_dir}/build/*` where the child contains `firmware.bin`
- else fallback to `${project}/.pio/build/<env>`

If `firmware.bin` is missing, the script raises an error.

### Partition and linker-script resolution

The script resolves PlatformIO sources in this order:

- `board_build.partitions` from the environment or global `[env]` section
- default `partitions.csv` in the project root for ESP32
- `board_build.ldscript` for ESP8266
- generated linker script in the build output if `ldscript.ld` was produced during build

It copies the resolved files into the version directory.

## flash.json generation

Every version directory ends with a `flash.json` file.

The generated payload is a dict like:

```json
{
  "board": "<board>",
  "soc": "<soc-family>",
  "version": "<version>",
  "flash_files": [
    { "offset": "0x1000", "file": "bootloader.bin" },
    { "offset": "0x8000", "file": "partitions.bin" },
    { "offset": "0x10000", "file": "firmware.bin" }
  ]
}
```

### PlatformIO offset rules

The script uses these defaults:

- ESP32 bootloader offset: `0x1000`
- ESP32-S3 bootloader offset: `0x0000`
- ESP32 partitions offset: `0x8000`
- ESP32 boot_app0 offset: `0xe000`
- firmware offset from `partitions.csv` or fallback logic
- filesystem offset from `partitions.csv`
- ESP8266 filesystem offset may also be derived from `_FS_start` in the linker script
- final ESP8266 fallback: `0x300000`

If no filesystem offset is found and there is a filesystem image, it logs a warning.

### ESP-IDF flash manifest handling

For ESP-IDF builds, the script reads `build/flasher_args.json` and copies all flash artifacts listed there. It classifies files by name and preserves a sensible output name, including:

- `bootloader.bin`
- `partitions.bin`
- `boot_app0.bin`
- `LittleFS.bin` for littlefs/spiffs/fatfs images when a matching build artifact is found
- other paths are mapped to `firmware.bin` when appropriate

It also adds an optional filesystem image when one is found in the build directory and a partition offset is available.

## Build log generation

The script writes a `build_log.md` file next to each generated version directory.

For PlatformIO it includes:

- executed commands
- stdout and stderr content
- warnings
- resolved build directory path

For ESP-IDF it stores:

- build log lines
- the source manifest path for `flasher_args.json`

## AWS sync behavior

The script supports two mutually exclusive sync modes:

### `--sync-aws`

This does a build first, then syncs only the generated project directory to AWS.

Behavior:

- creates remote destination: `<aws_target>/projects/<project-name>`
- uses `rsync -avz --update`
- excludes `.DS_Store`, `*.tmp`, `*.bak`, `.venv/`
- never deletes remote files
- supports `--aws-dry-run` to preview changes without copying

### `--only-sync-aws`

This skips the build and validates the full local `projects/` tree before syncing it.

Validation requirements:

- each project folder must contain `project.json`, `project_en.md`, and `project_nl.md`
- each project must contain at least one `flash.json` with a sibling `firmware.bin`

Then it uploads the entire local `projects/` directory to `<aws_target>/projects/`.

## CLI usage

From the repository root, or from any path using a relative/absolute argument:

```bash
python3 createProjectStructure.py <path-to-project>
```

Build and sync only the generated project:

```bash
python3 createProjectStructure.py <path-to-project> --sync-aws
```

Preview sync without copying:

```bash
python3 createProjectStructure.py <path-to-project> --sync-aws --aws-dry-run
```

Skip build and sync the whole local `projects/` folder:

```bash
python3 createProjectStructure.py <path-to-project> --only-sync-aws
```

Preview the full-folder sync only:

```bash
python3 createProjectStructure.py <path-to-project> --only-sync-aws --aws-dry-run
```

## Operational notes

- The script changes directory into the target project before building.
- It removes the existing `projects/<project-name>/` tree before regenerating it.
- Any env name containing `skip` is excluded from PlatformIO build and output generation.
- Missing `firmware.bin` is a hard error.
- `buildfs` errors are logged as warnings only.
- `--only-sync-aws` does not build anything; it only checks and syncs existing build output.

## Source-of-truth rule

If any behavior in this document conflicts with `createProjectStructure.py`, the Python code is authoritative. This markdown is only a summary of what the script currently implements.
