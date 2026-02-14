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

#define CHUNK_SIZE 256
#define TIMEOUT_MS 2000
static on_report_options_ptr on_report_options;

// --- globals ---
static io_stream_t src_stream;              // the stream that issued the $F> command
static stream_read_ptr read_backup;         // original hal.stream.read
static on_execute_realtime_ptr rt_backup;   // original realtime hook

static struct {
    vfs_file_t *file;
    const char *filename;
    uint32_t expected;
    uint32_t received;
    uint32_t crc32;
    bool active;

    uint16_t chunk_size;     // e.g. 256
    uint8_t  seq;           // chunk buffer sequence number, starts at 0

    uint8_t  buf[CHUNK_SIZE];// set to chunk_size
    uint16_t fill;          // number of bytes currently in buf

    uint32_t last_rx_ticks;  // for timeout check
    uint32_t timeout_ticks;  // computed from ms
} up;


// --- optional CRC32 helper ---
static uint32_t crc32_update(uint32_t crc, uint8_t data)
{
    crc ^= data;
    for (int i = 0; i < 8; i++)
        crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320UL : (crc >> 1);
    return crc;
}

static int32_t upload_read_stub (void)
{
    return -1; // parser sees "no input"
}

static void upload_stop (const char *msg)
{
    // restore parser read first (critical)
    hal.stream.read = read_backup;

    // restore realtime hook
    grbl.on_execute_realtime = rt_backup;

    // close file last
    if(up.file) {
        vfs_close(up.file);
        up.file = NULL;
    }

    up.active = false;

    if(msg) {
        src_stream.write("[MSG:");
        src_stream.write(msg);
        src_stream.write("]" ASCII_EOL);
    }
}
static void upload_pump (sys_state_t state)
{
    // chain original realtime processing
    if(rt_backup) rt_backup(state);

    if(!up.active)
        return;

    // ---- timeout check ----
    uint32_t now = hal.get_elapsed_ticks();
    if((now - up.last_rx_ticks) > up.timeout_ticks) {
        upload_stop("FUP TIMEOUT");
        return;
    }

    // ---- drain incoming bytes ----
    while(up.received < up.expected) {

        int16_t c = src_stream.read();  // read from the ORIGINAL stream (UART)
        if(c < 0)
            break; // no more bytes right now

        up.last_rx_ticks = now; // update activity

        uint8_t b = (uint8_t)c;

        // buffer
        up.buf[up.fill++] = b;
        up.received++;
        up.crc32 = crc32_update(up.crc32, b);

        // write chunk when full or at end
        if(up.fill == up.chunk_size || up.received == up.expected) {

            size_t want = up.fill;
            size_t wrote = vfs_write(up.buf, want, 1, up.file);
            if(wrote != want) {
                upload_stop("FUP SD WRITE ERROR");
                return;
            }

            // ACK this chunk
            char ack[48];
            snprintf(ack, sizeof(ack), "ACK,%u,%u", (unsigned)up.seq, (unsigned)want);
            src_stream.write(ack);
            src_stream.write(ASCII_EOL);

            up.seq++;
            up.fill = 0;
        }
    }

    // ---- done ----
    if(up.received == up.expected) {
        char done[96];
        snprintf(done, sizeof(done),
                 "FUP DONE bytes=%lu crc32=%08lX",
                 (unsigned long)up.received,
                 (unsigned long)(up.crc32 ^ 0xFFFFFFFFUL));

        // Tell host, then stop (restores hooks, closes file)
        src_stream.write("[MSG:");
        src_stream.write(done);
        src_stream.write("]" ASCII_EOL);
        src_stream.write("ok" ASCII_EOL);

        upload_stop(NULL);
        return;
    }
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


status_code_t file_upload_start (const char *fname, uint32_t size)
{
    if(up.active)
        return Status_InvalidStatement;

    // Must be mounted already if your VFS needs it (see note below)
    up.file = vfs_open(fname, "w");
    if(!up.file)
        return Status_FileOpenFailed;

    // capture the stream that issued the command (UART stream)
    src_stream = hal.stream;

    up.filename = fname;
    up.expected = size;
    up.received = 0;
    up.crc32 = 0xFFFFFFFFUL;
    up.active = true;

    up.chunk_size = CHUNK_SIZE;
    up.seq = 0;
    up.fill = 0;

    // timeout setup
    up.last_rx_ticks = hal.get_elapsed_ticks();

     up.timeout_ticks = TIMEOUT_MS; // "about 2 seconds"

    // stop parser from consuming binary
    read_backup = hal.stream.read;
    hal.stream.read = upload_read_stub;

    // install realtime pump
    rt_backup = grbl.on_execute_realtime;
    grbl.on_execute_realtime = upload_pump;

    // Ready message
    src_stream.write("[MSG:FUP RDY]" ASCII_EOL);

    return Status_OK;
}


static status_code_t sd_command_upload_start(sys_state_t state, char *args)
{
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

    char *size_str = ltrim(comma + 1);
    rtrim(size_str);

    if (*filename == '\0' || *size_str == '\0')
        return Status_InvalidStatement;

    char *endptr;
    unsigned long size = strtoul(size_str, &endptr, 10);
    if (*endptr != '\0' || size > UINT32_MAX)
        return Status_InvalidStatement;

    return file_upload_start(filename, (uint32_t)size);
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
        {"F>", sd_command_upload_start, {}, {.str = "upload to SD card. $F>=<filename>,<size>"}},
    };

    static sys_commands_t sdcard_commands = {
        .n_commands = sizeof(sdcard_command_list) / sizeof(sys_command_t),
        .commands = sdcard_command_list};

    on_report_options = grbl.on_report_options;
    grbl.on_report_options = onReportOptions;
    system_register_commands(&sdcard_commands);
}

#endif // FS_ENABLE & FS_SDCARD
