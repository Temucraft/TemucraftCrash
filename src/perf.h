// TemucraftCrash performance monitor: the parts that don't need Windows (frame statistics, time per category with
// nesting, the per-frame name tables, hitch / stats / interaction lines). Built into the DLL and into
// tests/test_core on Linux.
//
// Time is in "ticks" of a counter with a known frequency (QueryPerformanceCounter in the DLL). Everything here runs
// on the game's frame thread only (the DLL checks the thread before calling in), so there are no locks. Lines go
// out through an emit function (the DLL buffers them and writes them from another thread). No heap.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "core.h"

namespace tc {
namespace perf {

enum Cat {
    CAT_LUA,        // Lua scripts run by the game (FrameScript_Execute)
    CAT_FILES,      // MPQ file opens and reads
    CAT_TEXTURES,   // texture creation
    CAT_MODELS,     // M2 model creation
    CAT_CHARACTER,  // character model build (CCharacterComponent)
    CAT_CALL,       // the character create functions we time (SetSelectedRace and so on)
    CAT_PRESENT,    // GxScenePresent (drawing finished, waiting for the GPU / vsync)
    CAT_COUNT
};
const char* CatName(int cat);

struct Settings {
    double hitchMs;     // a frame this long or longer is a hitch
    double settleMs;    // after an interaction: settled after 3 frames in a row shorter than this
    unsigned windowSec; // a stats line every this many seconds
};
void DefaultSettings(Settings* s);
// The marker file's text: lines "hitch=80", "settle=25", "window=5" (others are ignored).
void ParseSettings(const char* text, size_t len, Settings* s);

// One output chunk: whole lines, each ending in \r\n.
typedef void (*EmitFn)(void* ctx, const char* text, size_t len);

uint64_t HashName(const char* s, size_t max);   // case blind, '/' same as '\'
uint64_t MixKey(uint64_t k);

// ---- names timed in one frame (Lua handlers, files, textures / models) ----

struct NameEntry {
    uint64_t key;
    int64_t ticks;
    uint32_t calls;
    char label[64];
};

template <unsigned SLOTS, unsigned MAXE>
struct NameTable {
    uint32_t gen;
    uint32_t slotGen[SLOTS];
    uint16_t slotIdx[SLOTS];
    uint64_t slotKey[SLOTS];
    NameEntry e[MAXE];
    unsigned count;
    int64_t lostTicks;   // time of names that didn't fit
    uint32_t lostCalls;

    void Init() {
        for (unsigned i = 0; i < SLOTS; i++) slotGen[i] = 0;
        gen = 1;
        count = 0;
        lostTicks = 0;
        lostCalls = 0;
    }
    void Reset() {
        if (++gen == 0) {
            for (unsigned i = 0; i < SLOTS; i++) slotGen[i] = 0;
            gen = 1;
        }
        count = 0;
        lostTicks = 0;
        lostCalls = 0;
    }
    // The entry for this key (new ones have calls 0, ticks 0 and an empty label). Null when the table is full.
    NameEntry* Get(uint64_t key, bool* isNew) {
        *isNew = false;
        unsigned i = static_cast<unsigned>(MixKey(key)) % SLOTS;
        for (unsigned probe = 0; probe < SLOTS; probe++, i = (i + 1) % SLOTS) {
            if (slotGen[i] != gen) {
                if (count >= MAXE) return nullptr;
                slotGen[i] = gen;
                slotKey[i] = key;
                slotIdx[i] = static_cast<uint16_t>(count);
                NameEntry& n = e[count++];
                n.key = key;
                n.ticks = 0;
                n.calls = 0;
                n.label[0] = 0;
                *isNew = true;
                return &n;
            }
            if (slotKey[i] == key) return &e[slotIdx[i]];
        }
        return nullptr;
    }
    void AddLost(int64_t ticks) {
        lostTicks += ticks;
        lostCalls++;
    }
    // indexes of the n entries with the most time, most first
    unsigned Top(unsigned n, unsigned* out) const {
        unsigned got = 0;
        for (unsigned i = 0; i < count; i++) {
            unsigned j = got < n ? got++ : n;
            if (j == n) {
                if (e[i].ticks <= e[out[n - 1]].ticks) continue;
                j = n - 1;
            }
            while (j > 0 && e[out[j - 1]].ticks < e[i].ticks) {
                out[j] = out[j - 1];
                j--;
            }
            out[j] = i;
        }
        return got;
    }
};

// ---- open file handles -> names (for reads that only have the handle) ----

template <unsigned N>
struct HandleNames {
    uint32_t handle[N];
    char name[N][64];
    void Init() {
        for (unsigned i = 0; i < N; i++) handle[i] = 0;
    }
    void Put(uint32_t h, const char* n) {
        if (!h) return;
        unsigned i = static_cast<unsigned>(MixKey(h)) % N;
        // up to 8 places; then the first one is overwritten (an old handle that was closed long ago)
        unsigned at = i;
        for (unsigned k = 0; k < 8; k++) {
            unsigned j = (i + k) % N;
            if (handle[j] == h || handle[j] == 0) {
                at = j;
                break;
            }
        }
        handle[at] = h;
        size_t len = 0;
        for (; n && n[len] && len < 63; len++) name[at][len] = n[len];
        name[at][len] = 0;
    }
    const char* Find(uint32_t h) const {
        if (!h) return nullptr;
        unsigned i = static_cast<unsigned>(MixKey(h)) % N;
        for (unsigned k = 0; k < 8; k++) {
            unsigned j = (i + k) % N;
            if (handle[j] == h) return name[j];
        }
        return nullptr;
    }
};

// ---- labels made once and kept (a frame's name is looked up the first time it runs, not every frame) ----

template <unsigned N>
struct LabelCache {
    uint64_t key[N];
    bool used[N];
    char label[N][64];
    void Init() {
        for (unsigned i = 0; i < N; i++) used[i] = false;
    }
    const char* Find(uint64_t k) const {
        unsigned i = static_cast<unsigned>(MixKey(k)) % N;
        for (unsigned p = 0; p < 4; p++) {
            unsigned j = (i + p) % N;
            if (used[j] && key[j] == k) return label[j];
        }
        return nullptr;
    }
    void Put(uint64_t k, const char* text) {
        unsigned i = static_cast<unsigned>(MixKey(k)) % N;
        unsigned at = i;
        for (unsigned p = 0; p < 4; p++) {
            unsigned j = (i + p) % N;
            if (!used[j] || key[j] == k) {
                at = j;
                break;
            }
        }
        used[at] = true;
        key[at] = k;
        size_t n = 0;
        for (; text && text[n] && n < 63; n++) label[at][n] = text[n];
        label[at][n] = 0;
    }
};

// ---- the monitor ----

enum { MAX_DEPTH = 64, HIST_BUCKETS = 2000,   // 0.1 ms steps to 100 ms, then 1 ms steps to 1100 ms
       WATCH_FRAMES = 40, MAX_SPANS = 8, FRAME_CALLS = 4 };

struct CallMark {
    int token;
    int64_t start;
    int64_t selfAll[CAT_COUNT];
};

struct Monitor {
    Settings set;
    int64_t freq;
    EmitFn emit;
    void* emitCtx;

    // the scope stack
    struct Scope {
        int cat;
        int64_t start;
        int64_t child;
    };
    Scope st[MAX_DEPTH];
    int sp;
    int catDepth[CAT_COUNT];

    // this frame
    uint64_t frameNo;
    int64_t lastFrame;
    int64_t selfFrame[CAT_COUNT];
    int64_t inclFrame[CAT_COUNT];
    uint32_t countFrame[CAT_COUNT];
    NameTable<512, 256> lua;
    NameTable<256, 128> files;
    NameTable<128, 64> assets;
    char frameCalls[FRAME_CALLS][64];
    unsigned frameCallCount;

    // since start
    int64_t selfAll[CAT_COUNT];

    // the stats window
    int64_t winStart, secStart;
    uint32_t winFrames, secFrames, winHitches;
    int64_t winTicks, winMax;
    double winMinFps;
    uint32_t hist[HIST_BUCKETS + 1];
    int64_t selfWin[CAT_COUNT];
    bool glueThisWindow;
    char screen[48];
    int64_t bgFileTicks;   // file work on other threads in this window (set by the DLL)
    uint32_t bgFileOps;

    // interactions
    struct Watch {
        bool on;
        bool sawFrame;
        char label[96];
        int64_t start;
        int64_t lastBad;   // end of the last frame at or over settle (0: none)
        unsigned n;
        unsigned calm;
        float ms[WATCH_FRAMES];
        bool slow[WATCH_FRAMES];
    } watch;
    struct Span {
        bool on;
        char label[48];
        int64_t start;
        uint64_t frame0;
        uint32_t hitches0;
    } spans[MAX_SPANS];
    uint32_t hitchesAll;

    void Init(int64_t frequency, const Settings& s, EmitFn fn, void* ctx);

    // scopes (Enter returns a token for Leave; -1 when the stack is full)
    int Enter(int cat, int64_t now);
    int64_t Leave(int token, int64_t now, bool* outermost);
    int Depth() const { return sp; }
    void Truncate(int depth);   // drops scopes above depth without counting them (a Lua error jumped over them)

    // the names (frame thread only; null when the table is full)
    NameEntry* LuaName(uint64_t key, bool* isNew) { return lua.Get(key, isNew); }
    NameEntry* FileName(uint64_t key, bool* isNew) { return files.Get(key, isNew); }
    NameEntry* AssetName(uint64_t key, bool* isNew) { return assets.Get(key, isNew); }

    // a whole frame is done (at the end of GxScenePresent)
    void FrameEnd(int64_t now);

    // interactions
    void Mark(const char* label, size_t len, int64_t now);
    void SpanToggle(const char* label, size_t len, int64_t now);
    void CallBegin(CallMark* m, int64_t now);
    void CallEnd(const CallMark& m, const char* label, int64_t now);
    void Screen(const char* name, size_t len, int64_t now);
    void GlueIdle() { glueThisWindow = true; }

    // internals
    double Ms(int64_t ticks) const { return freq ? static_cast<double>(ticks) * 1000.0 / static_cast<double>(freq) : 0.0; }
    void StartWatch(const char* label, int64_t start);
    void EndWatch(int64_t now, const char* why);
    void EmitHitch(double ms, int64_t ticks);
    void EmitStats(int64_t now);
    void Out(const Buf& b);
};

// "12.3" (one decimal, rounded)
void PutMs(Buf& b, double ms);

}  // namespace perf
}  // namespace tc
