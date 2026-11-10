// Stub backend: implements core_api.h without emulating the console.
//
// It exists so the iOS app can be built, sideloaded and tested end to end
// before an engine is linked. Unlike a pure mock, it runs the real front of
// the pipeline on the title you pick:
//   disc image -> XDVDFS -> default.xbe -> headers + sections into guest RAM
//   -> kernel imports resolved to HLE traps -> interpreter from the entry point
// and reports where execution stopped (first kernel call, or first opcode the
// reference interpreter lacks). After that it draws a test pattern that
// reacts to the controller and plays a tone while A is held.
// Left out when the app links the xemu libraries (see xemu_backend.cpp).
#if !defined(EMBER_BACKEND_XEMU)

#include "ember/core_api.h"

#include "../cpu/x86_state.h"
#include "../fs/byte_source.h"
#include "../fs/xiso.h"
#include "../kernel/xbe_loader.h"
#include "../mem/xbox_memory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace ember;

namespace {

constexpr double kTwoPi = 6.283185307179586;

// Single-producer / single-consumer ring of interleaved stereo frames.
class AudioRing {
public:
    explicit AudioRing(size_t frames) : buf_(frames * 2), capacity_(frames) {}

    void write(const int16_t* in, size_t frames) {
        const size_t w = write_.load(std::memory_order_relaxed);
        const size_t r = read_.load(std::memory_order_acquire);
        frames = std::min(frames, capacity_ - (w - r)); // drop on overflow rather than block the guest
        for (size_t i = 0; i < frames; ++i) {
            const size_t slot = (w + i) % capacity_;
            buf_[slot * 2]     = in[i * 2];
            buf_[slot * 2 + 1] = in[i * 2 + 1];
        }
        write_.store(w + frames, std::memory_order_release);
    }

    size_t read(int16_t* out, size_t frames) {
        const size_t r = read_.load(std::memory_order_relaxed);
        const size_t w = write_.load(std::memory_order_acquire);
        const size_t n = std::min(frames, w - r);
        for (size_t i = 0; i < n; ++i) {
            const size_t slot = (r + i) % capacity_;
            out[i * 2]     = buf_[slot * 2];
            out[i * 2 + 1] = buf_[slot * 2 + 1];
        }
        std::memset(out + n * 2, 0, (frames - n) * 2 * sizeof(int16_t));
        read_.store(r + n, std::memory_order_release);
        return n;
    }

private:
    std::vector<int16_t> buf_;
    size_t               capacity_;
    std::atomic<size_t>  write_{0}, read_{0};
};

// The stub draws on the CPU, so it caps its own internal resolution; a GPU
// backend honours the full requested scale.
constexpr float kStubMaxScale = 2.0f;

std::string describeStop(const RunResult& r) {
    char text[160];
    switch (r.reason) {
    case StopReason::KernelCall: {
        const char* name = kernelExportName(r.ordinal);
        std::snprintf(text, sizeof(text), "Ran %llu instructions, then called kernel export %u%s%s%s (no HLE kernel in the stub)",
                      static_cast<unsigned long long>(r.executed), r.ordinal, name ? " (" : "", name ? name : "", name ? ")" : "");
        break;
    }
    case StopReason::Unimplemented:
        std::snprintf(text, sizeof(text), "Ran %llu instructions, then hit opcode %02X at %08X (outside the reference interpreter)",
                      static_cast<unsigned long long>(r.executed), r.opcode, r.address);
        break;
    case StopReason::Fault:
        std::snprintf(text, sizeof(text), "Ran %llu instructions, then accessed %08X outside guest RAM",
                      static_cast<unsigned long long>(r.executed), r.address);
        break;
    case StopReason::Halt:
    case StopReason::Breakpoint:
        std::snprintf(text, sizeof(text), "Ran %llu instructions, then halted", static_cast<unsigned long long>(r.executed));
        break;
    default:
        std::snprintf(text, sizeof(text), "Still running after %llu instructions", static_cast<unsigned long long>(r.executed));
    }
    return text;
}

} // namespace

struct em_core {
    em_config   config{};
    std::string dataDir, cacheDir;

    XboxMemory memory;
    X86State   cpu;
    LoadedXbe  title;
    bool       titleLoaded = false;

    std::mutex            frameMutex;
    std::vector<uint32_t> front, back; // BGRA8 in memory = 0xAARRGGBB little-endian
    uint32_t              frontWidth = 0, frontHeight = 0;
    uint64_t              serial = 0;
    std::atomic<float>    requestedScale{1.0f};

    AudioRing audio{EM_AUDIO_SAMPLE_RATE / 4}; // 250 ms of headroom

    std::mutex   padMutex;
    em_pad_state pads[4] = {};

    std::mutex  statusMutex;
    std::string status = "No title loaded";

    std::thread       thread;
    std::atomic<bool> running{false}, paused{false};
    std::atomic<double> fps{0}, mips{0};
    void*  jitRx = nullptr;
    void*  jitRw = nullptr;
    size_t jitSize = 0;
    double tonePhase = 0;

    void run();
    void draw(uint32_t frame, const em_pad_state& pad);
    void setStatus(std::string text) {
        std::lock_guard<std::mutex> lock(statusMutex);
        status = std::move(text);
    }
};

void em_core::draw(uint32_t frame, const em_pad_state& p) {
    const float scale = std::clamp(requestedScale.load(), 1.0f, kStubMaxScale);
    const uint32_t w = uint32_t(std::lround(float(EM_NATIVE_WIDTH) * scale));
    const uint32_t h = uint32_t(std::lround(float(EM_NATIVE_HEIGHT) * scale));
    back.resize(size_t(w) * h);

    // Marker follows the left stick; it turns green while A is held.
    const int cx = int((0.5f + 0.45f * (p.lx / 32768.0f)) * w);
    const int cy = int((0.5f - 0.45f * (p.ly / 32768.0f)) * h);
    const int radius = int(h / 20);
    const uint32_t markerColor = p.analog[EM_ANALOG_A] ? 0xFF40D060u : 0xFFFFFFFFu;

    for (uint32_t y = 0; y < h; ++y) {
        uint32_t* row = &back[size_t(y) * w];
        const uint32_t gy = y * 150 / h;
        for (uint32_t x = 0; x < w; ++x) {
            // Scale-independent pattern with thin diagonal lines: aliasing on
            // them makes the effect of the render scale and filters visible.
            const uint32_t nx = x * EM_NATIVE_WIDTH / w, ny = y * EM_NATIVE_HEIGHT / h;
            const bool line = ((nx + ny * 3 + frame) % 64) < 2;
            const uint32_t gx = x * 110 / w;
            uint32_t r = 20 + gx / 3, g = 44 + gx, b = 30 + gy;
            if (line) { r = 235; g = 235; b = 235; }
            row[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
    for (int y = std::max(0, cy - radius); y < std::min<int>(h, cy + radius); ++y)
        for (int x = std::max(0, cx - radius); x < std::min<int>(w, cx + radius); ++x)
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= radius * radius) back[size_t(y) * w + x] = markerColor;

    std::lock_guard<std::mutex> lock(frameMutex);
    front.swap(back);
    frontWidth  = w;
    frontHeight = h;
    serial++;
}

void em_core::run() {
    using clock = std::chrono::steady_clock;

    // CPU: run the title from its entry point until the first thing the stub
    // cannot do, and say what that was.
    uint64_t executedTotal = 0;
    if (titleLoaded) {
        cpu = X86State{};
        cpu.eip = title.image.entryPoint;
        cpu.reg[ESP] = memory.size() - 0x1000; // scratch stack at the top of RAM
        const RunResult r = interpret(cpu, memory, 50'000'000);
        executedTotal = r.executed;
        setStatus(describeStop(r));
    }

    const auto frameTime = std::chrono::nanoseconds(16683350); // 59.94 Hz (NTSC)
    auto     deadline = clock::now(), window = clock::now();
    uint32_t frame = 0, framesInWindow = 0;
    std::vector<int16_t> mix((EM_AUDIO_SAMPLE_RATE / 60 + 1) * 2);
    mips.store(double(executedTotal) / 1e6);

    while (running.load(std::memory_order_acquire)) {
        if (paused.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            deadline = clock::now();
            continue;
        }
        em_pad_state p;
        {
            std::lock_guard<std::mutex> lock(padMutex);
            p = pads[0];
        }
        draw(frame, p);

        const size_t frames = EM_AUDIO_SAMPLE_RATE / 60;
        const double volume = p.analog[EM_ANALOG_A] / 255.0 * 6000.0; // pressure-sensitive, like the real button
        for (size_t i = 0; i < frames; ++i) {
            const int16_t v = static_cast<int16_t>(std::sin(tonePhase) * volume);
            tonePhase += kTwoPi * 440.0 / double(EM_AUDIO_SAMPLE_RATE);
            if (tonePhase > kTwoPi) tonePhase -= kTwoPi;
            mix[i * 2] = mix[i * 2 + 1] = v;
        }
        audio.write(mix.data(), frames);

        ++frame;
        ++framesInWindow;
        const auto now = clock::now();
        if (now - window >= std::chrono::seconds(1)) {
            fps.store(framesInWindow / std::chrono::duration<double>(now - window).count());
            framesInWindow = 0;
            window = now;
        }
        if (config.vsync) {
            deadline += frameTime;
            if (deadline < now) deadline = now; // fell behind: don't try to catch up
            std::this_thread::sleep_until(deadline);
        } else {
            std::this_thread::yield();
        }
    }
}

// --- C API -----------------------------------------------------------------

const char* em_core_backend_name(void) { return "stub"; }

em_core* em_core_create(const em_config* config) {
    if (!config) return nullptr;
    auto core = std::make_unique<em_core>();
    core->config = *config;
    if (config->data_dir) core->dataDir = config->data_dir;
    if (config->cache_dir) core->cacheDir = config->cache_dir;
    core->config.data_dir = core->config.cache_dir = nullptr; // caller's pointers are not retained
    if (!core->memory.init(config->ram_mb)) return nullptr;   // only 64 or 128 are accepted
    core->requestedScale.store(config->render_scale);
    return core.release();
}

void em_core_destroy(em_core* core) {
    if (!core) return;
    em_core_stop(core);
    delete core;
}

bool em_core_load_title(em_core* core, const char* path, char* err, size_t err_len) {
    auto fail = [&](const std::string& message) {
        if (err && err_len) std::snprintf(err, err_len, "%s", message.c_str());
        return false;
    };
    if (!core || !path) return fail("No title given");
    if (core->running.load()) return fail("Stop the running title first");

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path file(path);
    if (fs::is_directory(file, ec)) {
        bool found = false;
        for (const auto& entry : fs::directory_iterator(file, ec)) {
            std::string name = entry.path().filename().string();
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
            if (name == "default.xbe") {
                file = entry.path();
                found = true;
                break;
            }
        }
        if (!found) return fail("The folder has no default.xbe");
    }

    FileSource source;
    if (!source.open(file.string())) return fail("Could not open " + file.string());

    std::string error;
    core->titleLoaded = false;
    XisoReader disc;
    if (disc.open(source)) {
        XisoEntry entry;
        if (!disc.find("default.xbe", entry)) return fail("The disc image has no default.xbe");
        SliceSource xbe(source, disc.fileOffset(entry), entry.size);
        if (!loadXbe(xbe, core->memory, core->title, error)) return fail("default.xbe: " + error);
    } else if (!loadXbe(source, core->memory, core->title, error)) {
        return fail(error);
    }
    core->titleLoaded = true;
    core->setStatus("Loaded " + core->title.image.titleName);
    return true;
}

void em_core_start(em_core* core) {
    if (!core || core->running.exchange(true)) return;
    core->paused.store(false);
    core->thread = std::thread([core] { core->run(); });
}

void em_core_set_paused(em_core* core, bool paused) {
    if (core) core->paused.store(paused);
}

void em_core_stop(em_core* core) {
    if (!core || !core->running.exchange(false)) return;
    if (core->thread.joinable()) core->thread.join();
}

void em_core_set_jit_region(em_core* core, void* rx, void* rw, size_t size) {
    if (!core) return;
    core->jitRx = rx;
    core->jitRw = rw;
    core->jitSize = size;
}

void em_core_set_render_scale(em_core* core, float scale) {
    if (core && scale > 0) core->requestedScale.store(scale);
}

bool em_core_lock_frame(em_core* core, const uint8_t** bgra, uint32_t* width, uint32_t* height, uint64_t* serial) {
    if (!core) return false;
    core->frameMutex.lock();
    if (bgra) *bgra = reinterpret_cast<const uint8_t*>(core->front.data());
    if (width) *width = core->frontWidth;
    if (height) *height = core->frontHeight;
    if (serial) *serial = core->serial;
    return true;
}

void em_core_unlock_frame(em_core* core) {
    if (core) core->frameMutex.unlock();
}

void em_core_set_pad(em_core* core, int port, const em_pad_state* state) {
    if (!core || !state || port < 0 || port > 3) return;
    std::lock_guard<std::mutex> lock(core->padMutex);
    core->pads[port] = *state;
}

size_t em_core_read_audio(em_core* core, int16_t* out, size_t frames) {
    if (!out) return 0;
    if (!core) {
        std::memset(out, 0, frames * 2 * sizeof(int16_t));
        return 0;
    }
    return core->audio.read(out, frames);
}

void em_core_get_stats(em_core* core, em_stats* out) {
    if (!core || !out) return;
    std::memset(out, 0, sizeof(*out));
    out->fps        = core->fps.load();
    out->guest_mips = core->mips.load();
    out->ram_mb     = core->memory.size() >> 20;
    {
        std::lock_guard<std::mutex> lock(core->frameMutex);
        out->frame_width  = core->frontWidth;
        out->frame_height = core->frontHeight;
    }
    out->recompiler_active = false; // the stub has no recompiler, whatever region it was handed
    std::lock_guard<std::mutex> lock(core->statusMutex);
    std::snprintf(out->status, sizeof(out->status), "%s", core->status.c_str());
}

#endif // !EMBER_BACKEND_XEMU
