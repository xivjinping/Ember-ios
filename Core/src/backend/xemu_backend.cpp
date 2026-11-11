// Ember core API on top of xemu.
//
// Compiled only when the app is linked with the xemu libraries
// (EMBER_BACKEND_XEMU, set by build-deps/xemu-lib/ember-xemu.xcconfig);
// otherwise stub_backend.cpp provides the same functions. Everything it
// needs from xemu goes through Dependencies/xemu/ember/ember-xemu.h.
//
// xemu emulates the whole console, so it needs three files the user
// supplies, in Documents/Ember/System:
//   * the MCPX boot ROM   - the one file of exactly 512 bytes
//   * a BIOS image        - a .bin of 256 KB, 512 KB or 1 MB
//   * a hard-disk image   - a .qcow2 (or .img) file
// They are recognised by size and extension, so their names do not matter.
#if defined(EMBER_BACKEND_XEMU)

#include "ember/core_api.h"

#include "ember-xemu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

struct SystemFiles {
    std::string bootRom, bios, hdd;
    std::string missing() const {
        std::string list;
        auto add = [&](bool absent, const char* what) {
            if (!absent) return;
            if (!list.empty()) list += ", ";
            list += what;
        };
        add(bootRom.empty(), "boot ROM (512-byte file)");
        add(bios.empty(), "BIOS (.bin)");
        add(hdd.empty(), "hard disk (.qcow2)");
        return list;
    }
};

std::string lowerExtension(const fs::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
    return e;
}

SystemFiles findSystemFiles(const fs::path& dir) {
    SystemFiles found;
    std::error_code ec;
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec)) files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end()); // same choice every time if there are several
    for (const auto& file : files) {
        const auto size = fs::file_size(file, ec);
        if (ec) continue;
        const std::string ext = lowerExtension(file);
        if (file.filename() == "eeprom.bin") continue; // written by xemu itself
        if (size == 512) {
            if (found.bootRom.empty()) found.bootRom = file.string();
        } else if (ext == ".qcow2" || ext == ".img") {
            if (found.hdd.empty()) found.hdd = file.string();
        } else if (size == 256 * 1024 || size == 512 * 1024 || size == 1024 * 1024) {
            if (found.bios.empty()) found.bios = file.string();
        }
    }
    return found;
}

// Buttons: Ember uses the guest's wButtons layout plus 0..255 pressures;
// xemu wants one bit per button and 16-bit triggers.
void sendPad(int port, const em_pad_state& p) {
    uint16_t b = 0;
    auto digital = [&](unsigned mask, unsigned bit) { if (p.buttons & mask) b |= bit; };
    auto analog = [&](int index, unsigned bit) { if (p.analog[index] >= 0x20) b |= bit; };
    digital(EM_PAD_DPAD_UP, EMBER_BTN_DPAD_UP);
    digital(EM_PAD_DPAD_DOWN, EMBER_BTN_DPAD_DOWN);
    digital(EM_PAD_DPAD_LEFT, EMBER_BTN_DPAD_LEFT);
    digital(EM_PAD_DPAD_RIGHT, EMBER_BTN_DPAD_RIGHT);
    digital(EM_PAD_START, EMBER_BTN_START);
    digital(EM_PAD_BACK, EMBER_BTN_BACK);
    digital(EM_PAD_LEFT_THUMB, EMBER_BTN_LSTICK);
    digital(EM_PAD_RIGHT_THUMB, EMBER_BTN_RSTICK);
    analog(EM_ANALOG_A, EMBER_BTN_A);
    analog(EM_ANALOG_B, EMBER_BTN_B);
    analog(EM_ANALOG_X, EMBER_BTN_X);
    analog(EM_ANALOG_Y, EMBER_BTN_Y);
    analog(EM_ANALOG_BLACK, EMBER_BTN_BLACK);
    analog(EM_ANALOG_WHITE, EMBER_BTN_WHITE);

    int16_t axes[EMBER_AXIS_COUNT] = {};
    axes[EMBER_AXIS_LTRIG] = int16_t(p.analog[EM_ANALOG_LEFT_TRIGGER] * 32767 / 255);
    axes[EMBER_AXIS_RTRIG] = int16_t(p.analog[EM_ANALOG_RIGHT_TRIGGER] * 32767 / 255);
    axes[EMBER_AXIS_LSTICK_X] = p.lx;
    axes[EMBER_AXIS_LSTICK_Y] = p.ly;
    axes[EMBER_AXIS_RSTICK_X] = p.rx;
    axes[EMBER_AXIS_RSTICK_Y] = p.ry;
    ember_input_set(port, b, axes);
}

} // namespace

struct em_core {
    em_config   config{};
    fs::path    dataDir, systemDir;
    std::string discPath;

    std::mutex  statusMutex;
    std::string status = "No title loaded";

    // Shown until the machine has drawn something.
    std::vector<uint32_t> blank = std::vector<uint32_t>(size_t(EM_NATIVE_WIDTH) * EM_NATIVE_HEIGHT, 0xFF000000u);
    std::mutex            blankMutex;
    bool                  lockedMachineFrame = false;

    std::atomic<bool> jitAttached{false};

    // For the statistics: what the last locked frame looked like.
    std::atomic<uint64_t> frameSerial{0};
    std::atomic<uint32_t> frameWidth{EM_NATIVE_WIDTH}, frameHeight{EM_NATIVE_HEIGHT};
    uint64_t                              statsSerial = 0;
    std::chrono::steady_clock::time_point statsTime = std::chrono::steady_clock::now();

    void setStatus(std::string text) {
        std::lock_guard<std::mutex> lock(statusMutex);
        status = std::move(text);
    }
};

const char* em_core_backend_name(void) { return "xemu"; }

em_core* em_core_create(const em_config* config) {
    if (!config || (config->ram_mb != 64 && config->ram_mb != 128)) return nullptr;
    auto* core = new em_core;
    core->config = *config;
    core->dataDir = config->data_dir ? config->data_dir : ".";
    core->systemDir = core->dataDir / "System";
    core->config.data_dir = core->config.cache_dir = nullptr; // caller's pointers are not retained
    std::error_code ec;
    fs::create_directories(core->systemDir, ec); // so the folder is there to drop files into
    return core;
}

void em_core_destroy(em_core* core) {
    if (!core) return;
    em_core_stop(core);
    delete core;
}

bool em_core_load_title(em_core* core, const char* path, char* err, size_t err_len) {
    auto fail = [&](const char* message) {
        if (err && err_len) std::snprintf(err, err_len, "%s", message);
        return false;
    };
    if (!core || !path) return fail("No title given");
    em_title_info info;
    if (!em_probe_title(path, &info) || info.format != EM_TITLE_XISO) {
        return fail("The xemu core runs disc images (.iso). Loose .xbe files and folders are not supported.");
    }
    core->discPath = path;
    core->setStatus(std::string("Disc: ") + info.name);
    return true;
}

void em_core_start(em_core* core) {
    if (!core) return;
    if (ember_xemu_state() != EMBER_XEMU_IDLE) {
        core->setStatus("The console ran once already. Close Ember completely and open it again to start another session.");
        return;
    }
    const SystemFiles files = findSystemFiles(core->systemDir);
    const std::string missing = files.missing();
    if (!missing.empty()) {
        core->setStatus("Put these in Files > Ember > Ember > System: " + missing);
        return;
    }
    // QEMU has no interpreter for this: without executable memory it would
    // fail inside qemu_init and end the app.
    if (!core->jitAttached.load()) {
        core->setStatus("JIT is not active. Close the game, tap Enable JIT in the library (and keep Recompiler on in Settings), then start again.");
        return;
    }

    // Everything QEMU and xemu print goes to a file the user can send.
    const std::string log = (core->dataDir / "xemu-log.txt").string();
    const int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        ::dup2(fd, STDOUT_FILENO);
        ::dup2(fd, STDERR_FILENO);
        ::close(fd);
        std::setvbuf(stdout, nullptr, _IOLBF, 0);
    }

    const std::string settings = (core->dataDir / "xemu.toml").string();
    const std::string eeprom = (core->systemDir / "eeprom.bin").string();
    ember_xemu_config cfg = {};
    cfg.settings_path = settings.c_str();
    cfg.bootrom_path = files.bootRom.c_str();
    cfg.flashrom_path = files.bios.c_str();
    cfg.hdd_path = files.hdd.c_str();
    cfg.dvd_path = core->discPath.c_str();
    cfg.eeprom_path = eeprom.c_str();
    cfg.ram_mb = int(core->config.ram_mb);
    cfg.controllers = 1;
    // Print the CPU state whenever the guest resets: a boot that goes wrong
    // usually shows up as a reset loop.
    static const char* const extraArgs[] = {"-d", "cpu_reset", nullptr};
    cfg.extra_args = extraArgs;
    if (ember_xemu_start(&cfg)) {
        core->setStatus("Console starting. Log: Ember/xemu-log.txt");
    } else {
        core->setStatus("The console could not be started.");
    }
}

void em_core_set_paused(em_core* core, bool paused) {
    if (core) ember_xemu_set_paused(paused);
}

void em_core_stop(em_core* core) {
    if (core) ember_xemu_request_stop();
}

void em_core_set_jit_region(em_core* core, void* rx, void* rw, size_t size) {
    // QEMU's code generator writes through rw and the CPU runs from rx.
    if (!core) return;
    ember_xemu_set_code_buffer(rx, rw, size);
    core->jitAttached.store(rx && rw && size);
}

void em_core_set_render_scale(em_core*, float) {
    // Internal resolution is a property of the renderer, which does not exist yet.
}

bool em_core_lock_frame(em_core* core, const uint8_t** bgra, uint32_t* width, uint32_t* height, uint64_t* serial) {
    if (!core) return false;
    const uint8_t* pixels = nullptr;
    uint32_t w = 0, h = 0;
    uint64_t s = 0;
    if (ember_xemu_lock_frame(&pixels, &w, &h, &s)) {
        core->lockedMachineFrame = true;
        s += 1; // the blank frame below is serial 1
        core->frameSerial.store(s);
        core->frameWidth.store(w);
        core->frameHeight.store(h);
    } else {
        core->blankMutex.lock();
        core->lockedMachineFrame = false;
        pixels = reinterpret_cast<const uint8_t*>(core->blank.data());
        w = EM_NATIVE_WIDTH;
        h = EM_NATIVE_HEIGHT;
        s = 1;
    }
    if (bgra) *bgra = pixels;
    if (width) *width = w;
    if (height) *height = h;
    if (serial) *serial = s;
    return true;
}

void em_core_unlock_frame(em_core* core) {
    if (!core) return;
    if (core->lockedMachineFrame) {
        ember_xemu_unlock_frame();
    } else {
        core->blankMutex.unlock();
    }
}

void em_core_set_pad(em_core* core, int port, const em_pad_state* state) {
    if (!core || !state || port < 0 || port > 3) return;
    sendPad(port, *state);
}

size_t em_core_read_audio(em_core*, int16_t* out, size_t frames) {
    // xemu plays its audio through SDL directly; nothing passes through here yet.
    if (out) std::memset(out, 0, frames * 2 * sizeof(int16_t));
    return 0;
}

void em_core_get_stats(em_core* core, em_stats* out) {
    if (!core || !out) return;
    std::memset(out, 0, sizeof(*out));
    out->ram_mb = core->config.ram_mb;
    out->frame_width = core->frameWidth.load();
    out->frame_height = core->frameHeight.load();

    // The game's own frame rate (buffer flips), not how often the app copies
    // the picture. This is the number that shows real speed.
    out->fps = ember_xemu_game_fps();

    const int state = ember_xemu_state();
    out->recompiler_active = state == EMBER_XEMU_RUNNING;
    std::lock_guard<std::mutex> lock(core->statusMutex);
    switch (state) {
    case EMBER_XEMU_STARTING:
        std::snprintf(out->status, sizeof(out->status), "Building the console... (log: Ember/xemu-log.txt)");
        break;
    case EMBER_XEMU_RUNNING:
        // Once the game draws, the picture is proof enough; until then the
        // changing address shows the guest CPU is executing.
        if (out->fps > 0) break;
        std::snprintf(out->status, sizeof(out->status), "Console running. CPU at 0x%08llX.",
                      static_cast<unsigned long long>(ember_xemu_guest_pc()));
        break;
    case EMBER_XEMU_EXITED:
        std::snprintf(out->status, sizeof(out->status), "The console has shut down. Log: Ember/xemu-log.txt");
        break;
    default:
        std::snprintf(out->status, sizeof(out->status), "%s", core->status.c_str());
    }
}

#endif // EMBER_BACKEND_XEMU
