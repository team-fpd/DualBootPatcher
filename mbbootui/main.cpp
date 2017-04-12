/*
 * TWRP is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * TWRP is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with TWRP.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <cinttypes>
#include <cstring>

#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "mbcommon/string.h"
#include "mbcommon/version.h"
#include "mbdevice/json.h"
#include "mblog/logging.h"
#include "mbp/patcherconfig.h"
#include "mbutil/autoclose/archive.h"
#include "mbutil/copy.h"
#include "mbutil/directory.h"
#include "mbutil/file.h"
#include "mbutil/finally.h"
#include "mbutil/integer.h"
#include "mbutil/properties.h"
#include "mbutil/string.h"

#include <android/log.h>

#include "gui/console.hpp"
#include "gui/gui.h"
// #include "gui/gui.hpp"
#include "gui/pages.hpp"
#include "gui/objects.hpp"

#include "daemon_connection.h"
#include "data.hpp"
#include "twrp-functions.hpp"
#include "variables.h"

#include "config/config.hpp"

#define APPEND_TO_LOG               1

#define MAX_LOG_SIZE                (1024 * 1024) /* 1MiB */

#define MBBOOTUI_BASE_PATH          "/raw/cache/multiboot/bootui"
#define MBBOOTUI_LOG_PATH           MBBOOTUI_BASE_PATH "/exec.log"
#define MBBOOTUI_SCREENSHOTS_PATH   MBBOOTUI_BASE_PATH "/screenshots";
#define MBBOOTUI_SETTINGS_PATH      MBBOOTUI_BASE_PATH "/settings.bin"

#define MBBOOTUI_RUNTIME_PATH       "/mbbootui"
#define MBBOOTUI_THEME_PATH         MBBOOTUI_RUNTIME_PATH "/theme"

#define DEFAULT_PROP_PATH           "/default.prop"

#define DEVICE_JSON_PATH            "/device.json"

#define BOOL_STR(x)                 ((x) ? "true" : "false")

static mbp::PatcherConfig pc;

static bool redirect_output_to_file(const char *path, mode_t mode)
{
    int flags = O_WRONLY | O_CREAT;
#if APPEND_TO_LOG
    flags |= O_APPEND;
#else
    flags |= O_TRUNC;
#endif

    int fd = open(path, flags, mode);
    if (fd < 0) {
        return false;
    }

    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);

    if (fd > STDERR_FILENO) {
        close(fd);
    }

    return true;
}

static bool detect_device()
{
    std::vector<unsigned char> contents;
    if (!mb::util::file_read_all(DEVICE_JSON_PATH, &contents)) {
        LOGE("%s: Failed to read file: %s", DEVICE_JSON_PATH, strerror(errno));
        return false;
    }
    contents.push_back('\0');

    mb::device::JsonError error;

    if (!mb::device::device_from_json(
            reinterpret_cast<char *>(contents.data()), tw_device, error)) {
        LOGE("%s: Failed to load device", DEVICE_JSON_PATH);
        return false;
    }

    if (tw_device.validate() != 0) {
        LOGE("%s: Validation failed", DEVICE_JSON_PATH);
        return false;
    }

    return true;
}

static bool extract_theme(const std::string &path, const std::string &target,
                          const std::string &theme_name)
{
    mb::autoclose::archive in(archive_read_new(), archive_read_free);
    if (!in) {
        LOGE("%s: Out of memory when creating archive reader", __FUNCTION__);
        return false;
    }
    mb::autoclose::archive out(archive_write_disk_new(), archive_write_free);
    if (!out) {
        LOGE("%s: Out of memory when creating disk writer", __FUNCTION__);
        return false;
    }

    archive_read_support_format_zip(in.get());

    // Set up disk writer parameters. We purposely don't extract any file
    // metadata
    int flags = ARCHIVE_EXTRACT_SECURE_SYMLINKS
            | ARCHIVE_EXTRACT_SECURE_NODOTDOT;

    archive_write_disk_set_standard_lookup(out.get());
    archive_write_disk_set_options(out.get(), flags);

    if (archive_read_open_filename(in.get(), path.c_str(), 10240) != ARCHIVE_OK) {
        LOGE("%s: Failed to open file: %s",
             path.c_str(), archive_error_string(in.get()));
        return false;
    }

    archive_entry *entry;
    int ret;
    std::string target_path;

    std::string common_prefix("theme/common/");
    std::string theme_prefix("theme/");
    theme_prefix += theme_name;
    theme_prefix += '/';

    while (true) {
        ret = archive_read_next_header(in.get(), &entry);
        if (ret == ARCHIVE_EOF) {
            break;
        } else if (ret == ARCHIVE_RETRY) {
            LOGW("%s: Retrying header read", path.c_str());
            continue;
        } else if (ret != ARCHIVE_OK) {
            LOGE("%s: Failed to read header: %s",
                 path.c_str(), archive_error_string(in.get()));
            return false;
        }

        const char *path = archive_entry_pathname(entry);
        if (!path || !*path) {
            LOGE("%s: Header has null or empty filename", path);
            return false;
        }

        const char *suffix;

        if (mb_starts_with(path, common_prefix.c_str())) {
            suffix = path + common_prefix.size();
        } else if (mb_starts_with(path, theme_prefix.c_str())) {
            suffix = path + theme_prefix.size();
        } else {
            LOGV("Skipping: %s", path);
            continue;
        }

        // Build path
        target_path = target;
        if (target_path.back() != '/' && *suffix != '/') {
            target_path += '/';
        }
        target_path += suffix;

        LOGV("Extracting: %s -> %s", path, target_path.c_str());

        archive_entry_set_pathname(entry, target_path.c_str());

        // Extract file
        ret = archive_read_extract2(in.get(), entry, out.get());
        if (ret != ARCHIVE_OK) {
            LOGE("%s: %s", archive_entry_pathname(entry),
                 archive_error_string(in.get()));
            return false;
        }
    }

    if (archive_read_close(in.get()) != ARCHIVE_OK) {
        LOGE("%s: Failed to close file: %s",
             path.c_str(), archive_error_string(in.get()));
        return false;
    }

    return true;
}

static void wait_forever()
{
    while (true) {
        pause();
    }
}

static void load_other_config()
{
    // Get Android version (needed for pattern input)
    std::string value;
    mb::util::file_get_property("/raw/system/build.prop",
                                "ro.build.version.sdk", &value, "0");
    mb::util::str_to_snum(value.c_str(), 10, &tw_android_sdk_version);
}

static void log_startup()
{
    auto const &tw_input_blacklist =
            tw_device.tw_input_blacklist();
    auto const &tw_input_whitelist =
            tw_device.tw_input_whitelist();
    auto const &tw_brightness_path =
            tw_device.tw_brightness_path();
    auto const &tw_secondary_brightness_path =
            tw_device.tw_secondary_brightness_path();
    auto const &tw_battery_path =
            tw_device.tw_battery_path();
    auto const &tw_cpu_temp_path =
            tw_device.tw_cpu_temp_path();
    auto const &tw_graphics_backends =
            tw_device.tw_graphics_backends();

    LOGV("----------------------------------------");
    LOGV("Launching mbbootui (version %s)", mb::version());
    LOGV("----------------------------------------");
    LOGV("Configuration:");
    LOGV("- Flags:                        0x%" PRIx64, tw_device.tw_flags());

#define LOG_FLAG(FLAG) \
    if (tw_device.tw_flags() & mb::device::TwFlag::FLAG) { \
        LOGV("  - " # FLAG); \
    }
    LOG_FLAG(TW_TOUCHSCREEN_SWAP_XY);
    LOG_FLAG(TW_TOUCHSCREEN_FLIP_X);
    LOG_FLAG(TW_TOUCHSCREEN_FLIP_Y);
    LOG_FLAG(TW_GRAPHICS_FORCE_USE_LINELENGTH);
    LOG_FLAG(TW_SCREEN_BLANK_ON_BOOT);
    LOG_FLAG(TW_BOARD_HAS_FLIPPED_SCREEN);
    LOG_FLAG(TW_IGNORE_MAJOR_AXIS_0);
    LOG_FLAG(TW_IGNORE_MT_POSITION_0);
    LOG_FLAG(TW_IGNORE_ABS_MT_TRACKING_ID);
    LOG_FLAG(TW_NEW_ION_HEAP);
    LOG_FLAG(TW_NO_SCREEN_BLANK);
    LOG_FLAG(TW_NO_SCREEN_TIMEOUT);
    LOG_FLAG(TW_ROUND_SCREEN);
    LOG_FLAG(TW_NO_CPU_TEMP);
    LOG_FLAG(TW_QCOM_RTC_FIX);
    LOG_FLAG(TW_HAS_DOWNLOAD_MODE);
    LOG_FLAG(TW_PREFER_LCD_BACKLIGHT);
#undef LOG_FLAG

    const char *temp;
    switch (tw_device.tw_pixel_format()) {
    case mb::device::TwPixelFormat::DEFAULT:   temp = "DEFAULT";   break;
    case mb::device::TwPixelFormat::ABGR_8888: temp = "ABGR_8888"; break;
    case mb::device::TwPixelFormat::RGBX_8888: temp = "RGBX_8888"; break;
    case mb::device::TwPixelFormat::BGRA_8888: temp = "BGRA_8888"; break;
    case mb::device::TwPixelFormat::RGBA_8888: temp = "RGBA_8888"; break;
    default:                                   temp = "(unknown)"; break;
    }

    LOGV("- tw_pixel_format:              %s", temp);

    switch (tw_device.tw_force_pixel_format()) {
    case mb::device::TwForcePixelFormat::NONE:    temp = "NONE";      break;
    case mb::device::TwForcePixelFormat::RGB_565: temp = "RGB_565";   break;
    default:                                      temp = "(unknown)"; break;
    }

    LOGV("- tw_force_pixel_format:        %s", temp);

    LOGV("- tw_overscan_percent:          %d", tw_device.tw_overscan_percent());

    LOGV("- tw_input_blacklist:           %s", !tw_input_blacklist.empty() ? tw_input_blacklist.c_str() : "(none)");
    LOGV("- tw_input_whitelist:           %s", !tw_input_whitelist.empty() ? tw_input_whitelist.c_str() : "(none)");

    LOGV("- tw_default_x_offset:          %d", tw_device.tw_default_x_offset());
    LOGV("- tw_default_y_offset:          %d", tw_device.tw_default_y_offset());

    LOGV("- tw_brightness_path:           %s", !tw_brightness_path.empty() ? tw_brightness_path.c_str() : "(autodetect)");
    LOGV("- tw_secondary_brightness_path: %s", !tw_secondary_brightness_path.empty() ? tw_secondary_brightness_path.c_str() : "(none)");

    if (tw_device.tw_max_brightness() >= 0) {
        LOGV("- tw_max_brightness:            %d", tw_device.tw_max_brightness());
    } else {
        LOGV("- tw_max_brightness:            %s", "(autodetect)");
    }
    if (tw_device.tw_default_brightness() >= 0) {
        LOGV("- tw_default_brightness:        %d", tw_device.tw_default_brightness());
    } else {
        LOGV("- tw_default_brightness:        %s", "(autodetect)");
    }

    LOGV("- tw_battery_path:              %s", !tw_battery_path.empty() ? tw_battery_path.c_str() : "(none)");
    LOGV("- tw_cpu_temp_path:             %s", !tw_cpu_temp_path.empty() ? tw_cpu_temp_path.c_str() : "(none)");

    LOGV("- tw_resource_path:             %s", tw_resource_path.c_str());
    LOGV("- tw_settings_path:             %s", tw_settings_path.c_str());
    LOGV("- tw_screenshots_path:          %s", tw_screenshots_path.c_str());
    LOGV("- tw_theme_zip_path:            %s", tw_theme_zip_path.c_str());

    if (!tw_graphics_backends.empty()) {
        LOGV("- tw_graphics_backends:         (%zu backends)", tw_graphics_backends.size());
        for (auto const &backend : tw_graphics_backends) {
            LOGV("  - %s", backend.c_str());
        }
    }

    LOGV("- tw_android_sdk_version:       %d", tw_android_sdk_version);
}

static void usage(FILE *stream)
{
    fprintf(stream,
            "Usage: mbbootui <zip file>\n"
            "\n"
            "Options:\n"
            "  --no-log-file  Display output to stdout/stderr instead of log file\n"
            "  -h, --help     Display this help text\n");
}

int main(int argc, char *argv[])
{
    enum {
        OPT_NO_LOG_FILE = 1000,
    };

    static struct option long_options[] = {
        { "no-log-file", no_argument, 0, OPT_NO_LOG_FILE },
        { "help",        no_argument, 0, 'h'             },
        { 0, 0, 0, 0 }
    };

    int opt;
    int long_index = 0;
    bool no_log_file = false;

    while ((opt = getopt_long(argc, argv, "h", long_options, &long_index)) != -1) {
        switch (opt) {
        case OPT_NO_LOG_FILE:
            no_log_file = true;
            break;

        case 'h':
            usage(stdout);
            return EXIT_SUCCESS;

        default:
            usage(stderr);
            return EXIT_FAILURE;
        }
    }

    // There should be one other argument
    if (argc - optind != 1) {
        usage(stderr);
        return EXIT_FAILURE;
    }

    umask(0);

    if (!mb::util::mkdir_recursive(MBBOOTUI_BASE_PATH, 0755)) {
        LOGE("%s: Failed to create directory: %s",
             MBBOOTUI_BASE_PATH, strerror(errno));
        return EXIT_FAILURE;
    }

    // Rotate log if it is too large
    struct stat sb;
    if (stat(MBBOOTUI_LOG_PATH, &sb) == 0 && sb.st_size > MAX_LOG_SIZE) {
        rename(MBBOOTUI_LOG_PATH, MBBOOTUI_LOG_PATH ".prev");
    }

    if (!no_log_file && !redirect_output_to_file(MBBOOTUI_LOG_PATH, 0600)) {
        return EXIT_FAILURE;
    }

    signal(SIGPIPE, SIG_IGN);

    // Set paths
    tw_resource_path = MBBOOTUI_THEME_PATH;
    tw_settings_path = MBBOOTUI_SETTINGS_PATH;
    tw_screenshots_path = MBBOOTUI_SCREENSHOTS_PATH;
    // Disallow custom themes, which could manipulate variables in such as way
    // as to execute malicious code
    tw_theme_zip_path = "";

    if (!detect_device()) {
        return EXIT_FAILURE;
    }

    // Check if device supports the boot UI
    if (!tw_device.tw_supported()) {
        LOGW("Boot UI is not supported for the device");
        return EXIT_FAILURE;
    }

    // Load device configuration options
    load_other_config();

    log_startup();

    if (!extract_theme(argv[optind], MBBOOTUI_THEME_PATH,
                       tw_device.tw_theme())) {
        LOGE("Failed to extract theme");
        return EXIT_FAILURE;
    }

    // Connect to daemon
    if (!mbtool_connection.connect()) {
        LOGE("Failed to connect to mbtool");
        return EXIT_FAILURE;
    }
    mbtool_interface = mbtool_connection.interface();

    LOGV("Loading default values...");
    DataManager::SetDefaultValues();

    // Set daemon version
    std::string mbtool_version;
    mbtool_interface->version(&mbtool_version);
    DataManager::SetValue(VAR_TW_MBTOOL_VERSION, mbtool_version);

    LOGV("Loading graphics system...");
    if (gui_init() < 0) {
        LOGE("Failed to load graphics system");
        return EXIT_FAILURE;
    }

    LOGV("Loading resources...");
    gui_loadResources();

    // Set ROM ID. mbtool's ROM detection code doesn't fully trust the
    // "ro.multiboot.romid" property and will do some additional checks to
    // ensure that the value is correct.
    std::string rom_id;
    mbtool_interface->get_booted_rom_id(&rom_id);
    if (rom_id.empty()) {
        LOGW("Could not determine ROM ID");
    }
    DataManager::SetValue(VAR_TW_ROM_ID, rom_id);

    LOGV("Loading user settings...");
    DataManager::ReadSettingsFile();

    LOGV("Loading language...");
    PageManager::LoadLanguage(DataManager::GetStrValue(VAR_TW_LANGUAGE));
    GUIConsole::Translate_Now();

    LOGV("Fixing time...");
    TWFunc::Fixup_Time_On_Boot();

    LOGV("Loading main UI...");
    //gui_start();
    gui_startPage("autoboot", 1, 0);

    // Exit action
    std::string exit_action;
    DataManager::GetValue(VAR_TW_EXIT_ACTION, exit_action);
    std::vector<std::string> args = mb::util::split(exit_action, ",");

    // Save settings
    DataManager::Flush();

    if (args.size() > 0) {
        if (args[0] == "reboot") {
            std::string reboot_arg;
            if (args.size() > 1) {
                reboot_arg = args[1];
            }
            bool result;
            mbtool_interface->reboot(reboot_arg, &result);
            wait_forever();
        } else if (args[0] == "shutdown") {
            bool result;
            mbtool_interface->shutdown(&result);
            wait_forever();
        }
    }

    return EXIT_SUCCESS;
}

extern "C" {

static int log_bridge(int prio, const char *tag, const char *fmt, va_list ap)
{
    mb::log::LogLevel level = mb::log::LogLevel::Debug;

    switch (prio) {
    case ANDROID_LOG_ERROR:
        level = mb::log::LogLevel::Error;
        break;
    case ANDROID_LOG_WARN:
        level = mb::log::LogLevel::Warning;
        break;
    case ANDROID_LOG_INFO:
        level = mb::log::LogLevel::Info;
        break;
    case ANDROID_LOG_DEBUG:
        level = mb::log::LogLevel::Debug;
        break;
    case ANDROID_LOG_VERBOSE:
        level = mb::log::LogLevel::Verbose;
        break;
    }

    char newfmt[512];
    if (snprintf(newfmt, sizeof(newfmt), "[%s] %s", tag, fmt)
            >= (int) sizeof(newfmt)) {
        // Doesn't fit
        return -1;
    }

    mb::log::logv(level, newfmt, ap);
    return 0;
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    va_list ap;

    // TODO: Update when logging functions return the number of bytes written
    va_start(ap, fmt);
    int ret = vsnprintf(nullptr, 0, fmt, ap) + 1;
    va_end(ap);

    va_start(ap, fmt);
    int logret = log_bridge(prio, tag, fmt, ap);
    va_end(ap);

    return logret < 0 ? logret : ret;
}

void __android_log_assert(const char *cond, const char *tag,
                          const char *fmt, ...)
{
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        log_bridge(ANDROID_LOG_FATAL, tag, fmt, ap);
        va_end(ap);
    } else {
        if (cond) {
            __android_log_print(ANDROID_LOG_FATAL, tag,
                                "Assertion failed: %s", cond);
        } else {
            __android_log_print(ANDROID_LOG_FATAL, tag,
                                "Unspecified assertion failed");
        }
    }

    abort();
}

}
