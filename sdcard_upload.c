/*
    sdcard_upload.c - SD card file upload plugin

    Part of GrblHAL

  Jason Titcomb, 2026
  Public domain
  This code is distributed in the hope that it will be useful,
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
io_stream_t my_stream;
static on_state_change_ptr state_backup;

// forward declaration
static int32_t file_upload_read(void);

typedef struct
{
    vfs_file_t *file;       // target SD file
    const char *filename;   // for reporting
    uint32_t expected_size; // total bytes expected
    uint32_t received;      // bytes received so far
    uint32_t crc;           // optional CRC32
    bool active;            // whether an upload is in progress
    bool echo;              // whether to echo back received bytes for progress feedback
} upload_t;
static upload_t upload;

// --- optional CRC32 helper ---
static uint32_t crc32_update(uint32_t crc, uint8_t data)
{
    crc ^= data;
    for (int i = 0; i < 8; i++)
        crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320UL : (crc >> 1);
    return crc;
}

static void report_msg_char(char c)
{
    if (my_stream.write_char)
        my_stream.write_char(c);
}

static void report_msg(const char *msg, message_type_t type)
{
    // Forward messages to the original stream
    if (my_stream.write)
    {
        my_stream.write("[MSG:");
        my_stream.write(msg);
        my_stream.write("]" ASCII_EOL);
    }
}

static void upload_finish(void)
{
    // restore the stream handlers to their original state
    hal.stream = my_stream;
    grbl.on_state_change = state_backup;
    upload.active = false;
    if (upload.file)
        vfs_close(upload.file);
    upload.file = NULL;
    upload.filename = NULL;
    upload.expected_size = 0;
    upload.received = 0;
    upload.crc = 0;
}

static void on_state_change(sys_state_t state)
{
    if (upload.active && (state == STATE_IDLE || state == STATE_ALARM))
    {
        upload_finish();
    }
    // forward to original handler
    if (state_backup)
        state_backup(state);
}

static void final_report(void)
{
    char uploadmsg[64];
    snprintf(uploadmsg, sizeof(uploadmsg), "File: %s, Bytes: %lu, CRC32: %08lX", upload.filename, upload.received, upload.crc ^ 0xFFFFFFFFUL);
    report_msg(uploadmsg, Message_Info);
    my_stream.write("ok" ASCII_EOL);
    upload_finish();
}

static char *ltrim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

static void rtrim(char *s)
{
    char *end = s + strlen(s) - 1;
    while (end >= s && (*end == ' ' || *end == '\t'))
    {
        *end = '\0';
        end--;
    }
}

// --- called by hal.stream.read during upload ---
static int32_t file_upload_read(void)
{
    if (!upload.active)
        return -1;

    int16_t c = my_stream.read();
    if (c < 0)
        return -1; // nothing available

    // test for EOF character
    if (c == ASCII_EOT)
    {
        final_report();
        return -1;
    }

    uint8_t b = (uint8_t)c;
    upload.received++;

    upload.crc = crc32_update(upload.crc, b);

    // write to SD via VFS
    size_t written = vfs_write(&b, 1, 1, upload.file);
    if (written != 1)
    {
        // handle SD write error: abort upload
        report_msg("Upload failed: SD write error", Message_Error);
        upload_finish();
        return -1;
    }

    if (upload.echo)
    {
        // echo back received byte for progress feedback
        report_msg_char(b);
    }

    // finish condition
    if (upload.received >= upload.expected_size)
    {
        final_report();
        upload_finish();
    }
    return -1; // do not feed parser
}

// --- called by $FUP command to start upload ---
status_code_t file_upload_start(const char *fname, uint32_t size, bool echo)
{
    if (upload.active)
        return Status_InvalidStatement; // already uploading

    upload.file = vfs_open(fname, "w");
    if (!upload.file)
        return Status_FileOpenFailed;

    upload.filename = fname;
    upload.expected_size = size;
    upload.received = 0;
    upload.crc = 0xFFFFFFFFUL;
    upload.active = true;
    upload.echo = echo;

    // capture stream
    my_stream = hal.stream;
    int16_t c;
    my_stream.reset_read_buffer(); // flush any pending input
    hal.stream.read = file_upload_read; // redirect reads to our upload handler

    state_backup = grbl.on_state_change;
    grbl.on_state_change = on_state_change;

    report_msg("Upload ready", Message_Info);
    return Status_OK;
}

static status_code_t sd_command_upload_start(sys_state_t state, char *args)
{
    bool should_echo = false;

    if (state != STATE_IDLE)
        return Status_SystemGClock;

    char *start = args[0] == '=' ? args + 1 : args;
    start = ltrim(start);
    if (*start == '\0')
        return Status_InvalidStatement;

    char *comma = strchr(start, ',');
    if (!comma)
        return Status_InvalidStatement;

    *comma = '\0';
    char *filename = ltrim(start);
    rtrim(filename);

    char *rest = ltrim(comma + 1);
    char *comma2 = strchr(rest, ',');

    char *size_str;
    char *echo_str = NULL;

    if (comma2)
    {
        *comma2 = '\0';
        size_str = ltrim(rest);
        rtrim(size_str);

        echo_str = ltrim(comma2 + 1);
        rtrim(echo_str);
    }
    else
    {
        size_str = ltrim(rest);
        rtrim(size_str);
    }

    if (*filename == '\0' || *size_str == '\0')
        return Status_InvalidStatement;

    char *endptr;
    unsigned long size = strtoul(size_str, &endptr, 10);
    if (*endptr != '\0' || size > UINT32_MAX)
        return Status_InvalidStatement;

    if (echo_str)
    {
        if (strcmp(echo_str, "echo") == 0)
            should_echo = true;
        else
            return Status_InvalidStatement; // reject unknown third argument
    }

    return file_upload_start(filename, (uint32_t)size, should_echo);
}

static void onReportOptions(bool newopt)
{
    on_report_options(newopt);

    if (newopt)
        hal.stream.write(",SD_UPLOAD");
    else
        report_plugin("SDCARD_UPLOAD", "1.01");
}

void sdcard_upload_init(void)
{
    PROGMEM static const sys_command_t sdcard_command_list[] = {
        {"F>", sd_command_upload_start, {}, {.str = "upload to SD card. $F>=<filename>,<size>,[echo]"}},
    };

    static sys_commands_t sdcard_commands = {
        .n_commands = sizeof(sdcard_command_list) / sizeof(sys_command_t),
        .commands = sdcard_command_list};

    on_report_options = grbl.on_report_options;
    grbl.on_report_options = onReportOptions;
    system_register_commands(&sdcard_commands);
}

#endif // FS_ENABLE & FS_SDCARD
