/*
    sdcard_upload.c - SD card file upload plugin

    Part of GrblHAL

  Jason Titcomb, 2026
  Public domain
  This code is is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

  Add this to src\grbl\plugins_init.h
  #if SDCARD_UPLOAD_ENABLE
    extern void sdcard_upload_init (void);
    sdcard_upload_init();
  #endif


*/
#include "sdcard/sdcard.h"

#if FS_ENABLE & FS_SDCARD

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "grbl/system.h" // for sys.abort
#include "grbl/nuts_bolts.h"
#include "sdcard/fs_fatfs.h"

static on_report_options_ptr on_report_options;

// forward declaration
static int32_t file_upload_read(void);

typedef struct
{
    vfs_file_t *file;       // target SD file
    const char *filename;   // for reporting
    uint32_t expected_size; // total bytes expected
    uint32_t received;      // bytes received so far
    uint32_t crc;           // optional CRC32
    bool active;
} upload_t;
static upload_t upload;

// backup of original stream reader
static stream_read_ptr stream_read_backup;

// --- optional CRC32 helper ---
static uint32_t crc32_update(uint32_t crc, uint8_t data)
{
    crc ^= data;
    for (int i = 0; i < 8; i++)
        crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320UL : (crc >> 1);
    return crc;
}

// --- called by $FUP command to start upload ---
status_code_t file_upload(const char *fname, uint32_t size)
{
    if (upload.active)
        return Status_InvalidStatement; // already uploading

    upload.file = vfs_open(fname, "w");
    if (!upload.file)
        return Status_FileOpenFailed;

    upload.filename = fname;
    upload.expected_size = size;
    upload.received = 0;
    upload.crc = 0;
    upload.active = true;

    // override stream reader
    stream_read_backup = hal.stream.read;
    hal.stream.read = file_upload_read;

    return Status_OK;
}

static void final_report(const char *msg)
{
    hal.stream.read = stream_read_backup;
    char uploadmsg[64];
    snprintf(uploadmsg, sizeof(uploadmsg), "File: %s, Bytes: %lu, CRC32: %08lX", upload.filename, upload.received, upload.crc);
    report_message(uploadmsg, Message_Info);
    report_message(msg, Message_Info);
    if (upload.file)
        vfs_close(upload.file);

    upload.active = false;
}

// --- called by hal.stream.read during upload ---
static int32_t file_upload_read(void)
{
    if (!upload.active)
        return -1;

    // Abort upload if system abort or cancel is set
    if (sys.abort || sys.cancel)
    {
        if (upload.file)
            vfs_close(upload.file);

        upload.active = false;
        hal.stream.read = stream_read_backup;
        report_message("Upload aborted by user", Message_Warning);
        return -1;
    }

    int16_t c = stream_read_backup();
    if (c < 0)
        return -1; // nothing available

    uint8_t b = (uint8_t)c;
    upload.received++;
    upload.crc = crc32_update(upload.crc, b);

    // write to SD via VFS
    size_t written = vfs_write(&b, 1, 1, upload.file);
    if (written != 1)
    {
        // handle SD write error: abort upload
        if (upload.file)
            vfs_close(upload.file);

        upload.active = false;
        hal.stream.read = stream_read_backup;
        report_message("Upload failed: SD write error", Message_Error);
        return -1;
    }

    // test for EOF character (Ctrl-Z) to allow early termination of upload
    if (b == ASCII_EOT || b == 0x1A) // ASCII SUB (Ctrl-Z)
    {
        final_report("Upload terminated");
        return -1;
    }

    // finish condition
    if (upload.received >= upload.expected_size)
    {
        final_report("Upload complete");
    }

    return -1; // do not feed parser
}

static status_code_t sd_command_upload_end(sys_state_t state, char *args)
{
    if (!upload.active)
        return Status_InvalidStatement; // not uploading

    final_report("Upload complete");
    return Status_OK;
}

/// expects command arguments: "<filename>,<size>"
static status_code_t sd_command_upload_start(sys_state_t state, char *args)
{
    if (state != STATE_IDLE)
        return Status_SystemGClock;

    char *start = args[0] == '=' ? args + 1 : args;
    if (start[0] == '\0')
        return Status_InvalidStatement;

    char *comma = strchr(start, ',');
    if (comma == NULL || comma == start || comma[1] == '\0')
        return Status_InvalidStatement;

    if (strchr(comma + 1, ',') != NULL)
        return Status_InvalidStatement;

    *comma = '\0';
    char *filename = start;
    char *size_str = comma + 1;

    char *endptr = NULL;
    unsigned long size = strtoul(size_str, &endptr, 10);
    if (endptr == size_str || *endptr != '\0' || size > UINT32_MAX)
        return Status_InvalidStatement;

    return file_upload(filename, (uint32_t)size);
}

static void onReportOptions(bool newopt)
{
    on_report_options(newopt);

    if (newopt)
        hal.stream.write(",SD_UPLOAD");
    else
        report_plugin("SDCARD_UPLOAD", "1.0");
}

void sdcard_upload_init(void)
{
    PROGMEM static const sys_command_t sdcard_command_list[] = {
        {"FUP", sd_command_upload_start, {}, {.str = "upload to SD card. $FUP=<filename>,<size>"}},
        {"FUE", sd_command_upload_end, {.noargs = On}, {.str = "finish upload to SD card"}}
    };

    static sys_commands_t sdcard_commands = {
        .n_commands = sizeof(sdcard_command_list) / sizeof(sys_command_t),
        .commands = sdcard_command_list};

    on_report_options = grbl.on_report_options;
    grbl.on_report_options = onReportOptions;
    system_register_commands(&sdcard_commands);
}

#endif // FS_ENABLE & FS_SDCARD
