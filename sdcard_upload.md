# SD Card Upload Plugin

## Overview
The `sdcard_upload` plugin provides functionality for uploading files directly to an SD card connected to the grblHAL Teensy 4 platform. This enables users to transfer G-code or other files to the SD card for local execution or storage, bypassing the need for a direct USB or serial connection during file execution.

## Features
- Upload files to the SD card via supported interfaces (e.g., serial, USB, or network, depending on configuration).
- Integrates with grblHAL's plugin system for easy enable/disable and configuration.
- Supports file management operations such as listing, deleting, and overwriting files on the SD card.
- Provides status and error reporting for upload operations.

## Usage
1. **Enable the Plugin:**
   - Ensure the plugin is included in your build and enabled in your configuration.
2. **Connect SD Card:**
   - Insert a compatible SD card into the appropriate slot on your hardware.
3. **Upload Files:**
   - Use the supported interface (e.g., a serial terminal or web UI) to send files to the SD card. Refer to your firmware's documentation for specific commands or UI options.
4. **Manage Files:**
   - List, delete, or overwrite files as needed using the available commands.

## SD Card Upload Command

### $F> Command

To upload a file to the SD card, use the `$F>` command:

```
$F>=<filename>[,<size>]
```

- `<filename>`: The name of the file to create on the SD card.
- `<size>` (optional): The total size of the file in bytes. 
If omitted, the upload will end when a `%` character is received after recieving at least 10 characters so the possibly.

**Examples:**

- `$F>=test.gcode,1024` — Uploads a file named `test.gcode` with a known size of 1024 bytes.
- `$F>=test.gcode` — Uploads a file named `test.gcode` with unknown size, terminated by `%`.

After issuing the command, send the file data. The plugin will acknowledge each line with `ok` to support flow control and progress tracking.

### Reporting and Status

- On successful upload, a summary message is reported:
   - `File: <filename>, Bytes: <bytes_received>, Lines: <lines_received>, CRC32: <crc32>`
- If an error occurs (e.g., SD write error), an error message is reported in the format:
   - `[MSG:Upload failed: <reason>]`
- During upload, each received line is acknowledged with `ok`.
- The plugin also reports its presence in the options list as `,SD_UPLOAD` or via `report_plugin("SDCARD_UPLOAD", "1.01")`.

## Integration
- The plugin is implemented in `src/plugins/sdcard_upload.c`.
- It may require configuration in your main firmware or build system to enable.
- Works in conjunction with grblHAL's SD card and file system support.

## Limitations
- File size and format limitations depend on the SD card and file system used (typically FAT32).
- Upload speed may vary based on the interface and hardware.

## Troubleshooting
- Ensure the SD card is properly formatted and inserted.
- Check for error messages during upload operations.
- Consult the firmware logs or status reports for more details if uploads fail.

## License
This plugin is distributed under the same license as the main grblHAL project. See the COPYING file for details.
