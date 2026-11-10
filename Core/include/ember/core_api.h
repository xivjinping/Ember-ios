// Ember core — stable C API.
//
// The Obj-C++ bridge and Swift frontend talk to the emulation core only
// through this header. The bundled backend is a *stub*: it loads a real
// title (XISO -> default.xbe -> sections mapped into guest RAM), runs the
// interpreter until the first thing it cannot handle, and then shows a test
// pattern. A full engine implements the same functions. See docs/ARCHITECTURE.md.
#ifndef EMBER_CORE_API_H
#define EMBER_CORE_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { EM_CPU_INTERPRETER = 0, EM_CPU_RECOMPILER = 1 } em_cpu_mode;

typedef enum {
    EM_TITLE_UNKNOWN = 0,
    EM_TITLE_XBE     = 1, // loose executable
    EM_TITLE_XISO    = 2, // XDVDFS disc image (.iso / .xiso), trimmed or full redump
    EM_TITLE_FOLDER  = 3  // extracted disc: a folder containing default.xbe
} em_title_format;

enum { EM_NATIVE_WIDTH = 640, EM_NATIVE_HEIGHT = 480 };
enum { EM_AUDIO_SAMPLE_RATE = 48000, EM_AUDIO_CHANNELS = 2 }; // AC97 output format

typedef struct {
    em_cpu_mode cpu_mode;
    uint32_t    ram_mb;        // 64 (retail) or 128 (debug kit / upgraded consoles)
    float       render_scale;  // 1.0 = 640x480 ... 4.5 = 2880x2160
    bool        vsync;         // pace the guest to its video refresh
    bool        async_shaders; // compile pipelines off-thread, skip draws until ready
    const char* data_dir;      // HDD partitions, saves, EEPROM (Documents/Ember)
    const char* cache_dir;     // shader + pipeline caches (Library/Caches)
} em_config;

// Digital buttons, same bit layout as the guest's XINPUT_GAMEPAD.wButtons.
#define EM_PAD_DPAD_UP    0x0001u
#define EM_PAD_DPAD_DOWN  0x0002u
#define EM_PAD_DPAD_LEFT  0x0004u
#define EM_PAD_DPAD_RIGHT 0x0008u
#define EM_PAD_START      0x0010u
#define EM_PAD_BACK       0x0020u
#define EM_PAD_LEFT_THUMB 0x0040u
#define EM_PAD_RIGHT_THUMB 0x0080u

// The original controller's face buttons and triggers are pressure
// sensitive: each reports 0..255. Index into em_pad_state.analog.
enum {
    EM_ANALOG_A = 0, EM_ANALOG_B, EM_ANALOG_X, EM_ANALOG_Y,
    EM_ANALOG_BLACK, EM_ANALOG_WHITE, EM_ANALOG_LEFT_TRIGGER, EM_ANALOG_RIGHT_TRIGGER,
    EM_ANALOG_COUNT
};

typedef struct {
    uint16_t buttons;
    uint8_t  analog[EM_ANALOG_COUNT];
    int16_t  lx, ly, rx, ry; // -32768..32767, up is +y
} em_pad_state;

typedef struct {
    em_title_format format;
    bool            verified;   // true when the contents were parsed, false when only the extension matched
    uint32_t        title_id;   // from the XBE certificate; 0 if unknown
    uint32_t        region;     // certificate game-region flags
    uint64_t        size_bytes;
    char            name[128];  // UTF-8
    char            title_code[12]; // "MS-004" style publisher/number code, or ""
} em_title_info;

typedef struct {
    double   fps;
    double   guest_mips;
    uint32_t ram_mb;
    uint32_t frame_width, frame_height; // current internal resolution
    bool     recompiler_active;
    char     status[160];               // what the CPU is doing, e.g. where the stub stopped
} em_stats;

typedef struct em_core em_core;

// --- lifecycle -------------------------------------------------------------
em_core*    em_core_create(const em_config* config);
void        em_core_destroy(em_core* core);
const char* em_core_backend_name(void); // "stub" until a real engine is linked

bool em_core_load_title(em_core* core, const char* path, char* err, size_t err_len);
void em_core_start(em_core* core);
void em_core_set_paused(em_core* core, bool paused);
void em_core_stop(em_core* core);

// --- CPU -------------------------------------------------------------------
// Hands the backend an executable region prepared by the platform layer
// (Core/src/cpu/jit/jit_memory.h). Without one the recompiler stays off.
void em_core_set_jit_region(em_core* core, void* rx, void* rw, size_t size);

// --- video -----------------------------------------------------------------
// May be called while running; takes effect on the next frame.
void em_core_set_render_scale(em_core* core, float scale);

// CPU-side frame access used by the stub. A GPU backend renders into its own
// Metal texture and the bridge returns that instead (zero copy).
bool em_core_lock_frame(em_core* core, const uint8_t** bgra, uint32_t* width, uint32_t* height, uint64_t* serial);
void em_core_unlock_frame(em_core* core);

// --- input / audio ---------------------------------------------------------
void em_core_set_pad(em_core* core, int port, const em_pad_state* state); // port 0..3

// Real-time safe. Writes `frames` interleaved stereo frames, zero-filling on
// underrun. Returns frames actually read.
size_t em_core_read_audio(em_core* core, int16_t* out, size_t frames);

// --- misc ------------------------------------------------------------------
void em_core_get_stats(em_core* core, em_stats* out);
bool em_probe_title(const char* path, em_title_info* out);

#ifdef __cplusplus
}
#endif
#endif // EMBER_CORE_API_H
