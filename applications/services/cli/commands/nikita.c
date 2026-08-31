/**
 * The firmware side of the Nikita ecosystem.
 *
 * The Nikita agents (nikita-IOS over BLE, nikita-qflipper over USB, and the
 * nikita-flipper-bridge in between) all end up talking to this device. Two
 * things they need are awkward to get today: a single, parseable snapshot of
 * what the device is, and a place to keep durable user facts that does not
 * belong to one client. Both live here, so a fact remembered on the phone is
 * the same fact the desktop reads back.
 */

#include "../cli_main_commands.h"

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_info.h>
#include <furi_hal_version.h>
#include <furi_hal_power.h>
#include <furi_hal_bt.h>
#include <toolbox/args.h>
#include <toolbox/version.h>
#include <toolbox/stream/stream.h>
#include <toolbox/stream/file_stream.h>
#include <storage/storage.h>

#define NIKITA_DIR         EXT_PATH("nikita")
#define NIKITA_MEMORY_FILE NIKITA_DIR "/memory.txt"

// The folders the ecosystem expects to find on a Nikita device.
static const char* const nikita_dirs[] = {
    NIKITA_DIR,
    NIKITA_DIR "/artifacts",
    NIKITA_DIR "/scripts",
};

// A remembered fact is one line. Keep both the line and the file bounded so a
// runaway agent can't eat the card or the command's stack.
#define NIKITA_MEMORY_MAX_LINES 64
#define NIKITA_MEMORY_MAX_LINE  256

static void nikita_print_usage(FuriString* args) {
    cli_print_usage("nikita", "<info|init|memory>", furi_string_get_cstr(args));
    printf("\r\n"
           "  nikita info                 device snapshot for the agent\r\n"
           "  nikita init                 create /ext/nikita on the SD card\r\n"
           "  nikita memory               list remembered facts\r\n"
           "  nikita memory add <text>    remember one fact\r\n"
           "  nikita memory forget <n>    drop fact number <n>\r\n"
           "  nikita memory clear         drop all of them\r\n");
}

static bool nikita_ensure_dirs(Storage* storage) {
    bool ok = true;
    for(size_t i = 0; i < COUNT_OF(nikita_dirs); i++) {
        FS_Error error = storage_common_mkdir(storage, nikita_dirs[i]);
        if(error != FSE_OK && error != FSE_EXIST) ok = false;
    }
    return ok;
}

/** `nikita info` -- everything the agent would otherwise need five calls for. */
static void nikita_info(Storage* storage) {
    const Version* ver = furi_hal_version_get_firmware_version();

    printf("firmware_origin  : %s\r\n", ver ? version_get_firmware_origin(ver) : "unknown");
    printf("firmware_version : %s\r\n", ver ? version_get_version(ver) : "unknown");
    printf("firmware_branch  : %s\r\n", ver ? version_get_gitbranch(ver) : "unknown");
    printf("firmware_commit  : %s\r\n", ver ? version_get_githash(ver) : "unknown");
    printf("firmware_build   : %s\r\n", ver ? version_get_builddate(ver) : "unknown");

    const char* name = furi_hal_version_get_name_ptr();
    printf("device_name      : %s\r\n", name ? name : "unknown");
    printf("hardware_target  : f%d\r\n", furi_hal_version_get_hw_target());

    uint16_t api_major, api_minor;
    furi_hal_info_get_api_version(&api_major, &api_minor);
    printf("api_version      : %d.%d\r\n", api_major, api_minor);

    printf("battery_pct      : %d\r\n", furi_hal_power_get_pct());
    printf("charging         : %s\r\n", furi_hal_power_is_charging() ? "yes" : "no");
    printf("bluetooth        : %s\r\n", furi_hal_bt_is_active() ? "on" : "off");

    uint64_t total = 0, free = 0;
    if(storage_common_fs_info(storage, STORAGE_EXT_PATH_PREFIX, &total, &free) == FSE_OK) {
        printf("sd_total_kb      : %lu\r\n", (unsigned long)(total / 1024));
        printf("sd_free_kb       : %lu\r\n", (unsigned long)(free / 1024));
    } else {
        printf("sd_total_kb      : none\r\n");
        printf("sd_free_kb       : none\r\n");
    }

    printf(
        "nikita_dir       : %s\r\n",
        storage_common_stat(storage, NIKITA_DIR, NULL) == FSE_OK ? NIKITA_DIR : "missing");
}

/** Walk memory.txt line by line. Returns the number of lines seen. */
static size_t nikita_memory_walk(
    Storage* storage,
    void (*on_line)(size_t index, FuriString* line, void* ctx),
    void* ctx) {
    Stream* stream = file_stream_alloc(storage);
    size_t index = 0;

    if(file_stream_open(stream, NIKITA_MEMORY_FILE, FSAR_READ, FSOM_OPEN_EXISTING)) {
        FuriString* line = furi_string_alloc();
        while(stream_read_line(stream, line)) {
            furi_string_trim(line);
            if(furi_string_empty(line)) continue;
            index++;
            if(on_line) on_line(index, line, ctx);
        }
        furi_string_free(line);
    }

    file_stream_close(stream);
    stream_free(stream);
    return index;
}

static void nikita_memory_print_line(size_t index, FuriString* line, void* ctx) {
    UNUSED(ctx);
    printf("%u. %s\r\n", (unsigned)index, furi_string_get_cstr(line));
}

typedef struct {
    size_t skip;
    Stream* out;
} NikitaMemoryRewrite;

static void nikita_memory_rewrite_line(size_t index, FuriString* line, void* ctx) {
    NikitaMemoryRewrite* rewrite = ctx;
    if(index == rewrite->skip) return;
    stream_write_format(rewrite->out, "%s\n", furi_string_get_cstr(line));
}

static void nikita_memory_list(Storage* storage) {
    if(nikita_memory_walk(storage, nikita_memory_print_line, NULL) == 0) {
        printf("Nothing remembered yet.\r\n");
    }
}

static void nikita_memory_add(Storage* storage, FuriString* args) {
    furi_string_trim(args);
    if(furi_string_empty(args)) {
        cli_print_usage("nikita memory add", "<text>", "");
        return;
    }
    if(furi_string_size(args) > NIKITA_MEMORY_MAX_LINE) {
        printf("Too long: one fact is at most %d characters.\r\n", NIKITA_MEMORY_MAX_LINE);
        return;
    }
    if(nikita_memory_walk(storage, NULL, NULL) >= NIKITA_MEMORY_MAX_LINES) {
        printf("Memory is full (%d facts). Forget something first.\r\n", NIKITA_MEMORY_MAX_LINES);
        return;
    }
    // A fact is one line, so a pasted newline would silently become two.
    furi_string_replace_all(args, "\n", " ");
    furi_string_replace_all(args, "\r", " ");

    if(!nikita_ensure_dirs(storage)) {
        printf("Cannot write to %s. Is the SD card mounted?\r\n", NIKITA_DIR);
        return;
    }

    Stream* stream = file_stream_alloc(storage);
    if(file_stream_open(stream, NIKITA_MEMORY_FILE, FSAR_WRITE, FSOM_OPEN_APPEND)) {
        stream_write_format(stream, "%s\n", furi_string_get_cstr(args));
        printf("Remembered.\r\n");
    } else {
        printf("Cannot open %s.\r\n", NIKITA_MEMORY_FILE);
    }
    file_stream_close(stream);
    stream_free(stream);
}

static void nikita_memory_forget(Storage* storage, FuriString* args) {
    int index = 0;
    if(!args_read_int_and_trim(args, &index) || index < 1) {
        cli_print_usage("nikita memory forget", "<n>", furi_string_get_cstr(args));
        return;
    }

    size_t total = nikita_memory_walk(storage, NULL, NULL);
    if((size_t)index > total) {
        printf("There is no fact %d (%u remembered).\r\n", index, (unsigned)total);
        return;
    }

    // Rewrite into a temporary file, then swap: a power loss mid-write must not
    // take the whole memory with it.
    const char* temp_path = NIKITA_MEMORY_FILE ".tmp";
    Stream* out = file_stream_alloc(storage);
    if(!file_stream_open(out, temp_path, FSAR_WRITE, FSOM_CREATE_ALWAYS)) {
        printf("Cannot write to %s.\r\n", NIKITA_DIR);
        file_stream_close(out);
        stream_free(out);
        return;
    }

    NikitaMemoryRewrite rewrite = {.skip = (size_t)index, .out = out};
    nikita_memory_walk(storage, nikita_memory_rewrite_line, &rewrite);
    file_stream_close(out);
    stream_free(out);

    storage_common_remove(storage, NIKITA_MEMORY_FILE);
    if(storage_common_rename(storage, temp_path, NIKITA_MEMORY_FILE) == FSE_OK) {
        printf("Forgotten.\r\n");
    } else {
        printf("Could not replace %s.\r\n", NIKITA_MEMORY_FILE);
    }
}

static void nikita_memory_clear(Storage* storage) {
    FS_Error error = storage_common_remove(storage, NIKITA_MEMORY_FILE);
    if(error == FSE_OK || error == FSE_NOT_EXIST) {
        printf("Memory cleared.\r\n");
    } else {
        printf("Could not clear %s.\r\n", NIKITA_MEMORY_FILE);
    }
}

static void nikita_memory(Storage* storage, FuriString* args) {
    FuriString* subcommand = furi_string_alloc();

    if(!args_read_string_and_trim(args, subcommand)) {
        nikita_memory_list(storage);
    } else if(furi_string_cmp(subcommand, "add") == 0) {
        nikita_memory_add(storage, args);
    } else if(furi_string_cmp(subcommand, "forget") == 0) {
        nikita_memory_forget(storage, args);
    } else if(furi_string_cmp(subcommand, "clear") == 0) {
        nikita_memory_clear(storage);
    } else {
        cli_print_usage("nikita memory", "<add|forget|clear>", furi_string_get_cstr(subcommand));
    }

    furi_string_free(subcommand);
}

static void execute(PipeSide* pipe, FuriString* args, void* context) {
    UNUSED(pipe);
    UNUSED(context);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* command = furi_string_alloc();

    if(!args_read_string_and_trim(args, command) || furi_string_cmp(command, "info") == 0) {
        nikita_info(storage);
    } else if(furi_string_cmp(command, "init") == 0) {
        if(nikita_ensure_dirs(storage)) {
            printf("%s is ready.\r\n", NIKITA_DIR);
        } else {
            printf("Could not create %s. Is the SD card mounted?\r\n", NIKITA_DIR);
        }
    } else if(furi_string_cmp(command, "memory") == 0) {
        nikita_memory(storage, args);
    } else {
        nikita_print_usage(command);
    }

    furi_string_free(command);
    furi_record_close(RECORD_STORAGE);
}

CLI_COMMAND_INTERFACE(nikita, execute, CliCommandFlagParallelSafe, 3072, CLI_APPID);
