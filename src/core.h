// TemucraftCrash: the parts that don't need Windows (text building, ring buffers, name lookups, the report
// layout). They are built into the DLL and also into tests/test_core on Linux, so they can be tested there.
//
// Nothing here uses the heap: a crash report has to work when the game's heap is broken.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace tc {

// ---- text into a fixed buffer ----

struct Buf {
    char* p;
    size_t cap;
    size_t len;
    bool full;

    Buf(char* buffer, size_t size) : p(buffer), cap(size), len(0), full(false) {
        if (cap) p[0] = 0;
    }
    void ch(char c);
    void str(const char* s);
    void strn(const char* s, size_t n);   // at most n chars, stops at 0
    void line(const char* s) { str(s); nl(); }
    void nl() { str("\r\n"); }
    void hex(uint32_t v, int digits = 8); // no 0x
    void hex0x(uint32_t v, int digits = 8) { str("0x"); hex(v, digits); }
    void dec(uint64_t v);
    void sdec(int64_t v);
    void padTo(size_t column);            // spaces up to this column of the current line
    void seconds(uint32_t ms);            // "1.234"
    void size(uint64_t bytes);            // "1.5 MB", "312 bytes"
    const char* c_str() const { return p; }
};

// ---- names ----

// The Wow.exe function an address is in ("NetClient::ProcessMessage"), from the embedded list of the decompiled
// client. offset gets the distance from the start. Null when the list doesn't know the address.
const char* WowFunction(uint32_t address, uint32_t* offset);
unsigned WowFunctionCount();

// "SMSG_GOSSIP_MESSAGE" for 0x17D; null when unknown.
const char* OpcodeName(uint32_t opcode);

// "ACCESS_VIOLATION" for 0xC0000005; null when unknown.
const char* ExceptionName(uint32_t code);

// ---- ring buffers (lock free: many writers, one reader at crash time) ----

enum { HEAD_BYTES = 16 };

struct PacketEntry {
    uint32_t tick;   // GetTickCount when it went through
    uint32_t opcode;
    uint32_t size;   // bytes after the opcode
    uint8_t headLen; // how many of the first bytes after the opcode are kept (server packets only)
    uint8_t head[HEAD_BYTES];
};

template <unsigned N>
struct PacketRing {
    struct Slot {
        uint32_t seq;  // number of the entry + 1, 0 while it is being written
        PacketEntry e;
    };
    uint32_t next;
    Slot slots[N];

    // head: the first bytes after the opcode (at most HEAD_BYTES are kept), or null
    void Push(uint32_t tick, uint32_t opcode, uint32_t size, const uint8_t* head = nullptr, unsigned headLen = 0) {
        uint32_t i = __atomic_fetch_add(&next, 1u, __ATOMIC_RELAXED);
        Slot& s = slots[i % N];
        __atomic_store_n(&s.seq, 0u, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        s.e.tick = tick;
        s.e.opcode = opcode;
        s.e.size = size;
        if (!head) headLen = 0;
        if (headLen > HEAD_BYTES) headLen = HEAD_BYTES;
        s.e.headLen = static_cast<uint8_t>(headLen);
        for (unsigned j = 0; j < headLen; j++) s.e.head[j] = head[j];  // bytes past headLen are never shown
        __atomic_store_n(&s.seq, i + 1, __ATOMIC_RELEASE);
    }

    // Copies up to max entries, newest first. Entries being written right now are left out.
    unsigned Snapshot(PacketEntry* out, unsigned max) const {
        uint32_t n = __atomic_load_n(&next, __ATOMIC_ACQUIRE);
        unsigned got = 0;
        for (uint32_t k = 0; k < N && k < n && got < max; k++) {
            uint32_t id = n - 1 - k;
            const Slot& s = slots[id % N];
            uint32_t before = __atomic_load_n(&s.seq, __ATOMIC_ACQUIRE);
            if (before != id + 1) continue;
            PacketEntry copy = s.e;
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (__atomic_load_n(&s.seq, __ATOMIC_RELAXED) != before) continue;
            out[got++] = copy;
        }
        return got;
    }
    uint32_t Total() const { return __atomic_load_n(&next, __ATOMIC_ACQUIRE); }
};

struct TextEntry {
    uint32_t tick;
    char text[480];
};

template <unsigned N>
struct TextRing {
    struct Slot {
        uint32_t seq;
        TextEntry e;
    };
    uint32_t next;
    Slot slots[N];

    void Push(uint32_t tick, const char* text, size_t len) {
        uint32_t i = __atomic_fetch_add(&next, 1u, __ATOMIC_RELAXED);
        Slot& s = slots[i % N];
        __atomic_store_n(&s.seq, 0u, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        s.e.tick = tick;
        size_t n = len < sizeof(s.e.text) - 1 ? len : sizeof(s.e.text) - 1;
        for (size_t j = 0; j < n; j++) s.e.text[j] = text[j] ? text[j] : ' ';
        s.e.text[n] = 0;
        __atomic_store_n(&s.seq, i + 1, __ATOMIC_RELEASE);
    }

    unsigned Snapshot(TextEntry* out, unsigned max) const {
        uint32_t n = __atomic_load_n(&next, __ATOMIC_ACQUIRE);
        unsigned got = 0;
        for (uint32_t k = 0; k < N && k < n && got < max; k++) {
            uint32_t id = n - 1 - k;
            const Slot& s = slots[id % N];
            uint32_t before = __atomic_load_n(&s.seq, __ATOMIC_ACQUIRE);
            if (before != id + 1) continue;
            out[got] = s.e;
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (__atomic_load_n(&s.seq, __ATOMIC_RELAXED) != before) continue;
            got++;
        }
        return got;
    }
    uint32_t Total() const { return __atomic_load_n(&next, __ATOMIC_ACQUIRE); }
};

// ---- characters and races (for crashes with the custom races) ----

enum { MAX_ENUM_CHARS = 12, MAX_CREATE_RACES = 48, MAX_DBC_COUNTS = 8 };

struct CharSummary {
    char name[24];
    uint8_t race, cls, gender, skin, face, hairStyle, hairColor, facialHair, level;
};

// The last character list from the server (SMSG_CHAR_ENUM).
struct CharList {
    uint32_t tick;
    uint8_t count;     // characters the packet says it has
    uint8_t parsed;    // characters read before the data ran out (at most MAX_ENUM_CHARS)
    bool complete;     // every character was read and the sizes added up
    CharSummary c[MAX_ENUM_CHARS];
};

// The last character the client asked to create (CMSG_CHAR_CREATE).
struct CharCreate {
    uint32_t tick;
    bool valid;
    CharSummary c;
    uint8_t outfit;
};

// Both read only inside [data, data + size); false when the packet is too short or malformed.
bool ParseCharEnum(const uint8_t* data, size_t size, CharList* out);
bool ParseCharCreate(const uint8_t* data, size_t size, CharCreate* out);

// One writer (the game's network code on the main thread), one reader at crash time.
template <class T>
struct Latest {
    uint32_t seq;  // odd while it is being written
    T v;

    void Store(const T& value) {
        uint32_t s = __atomic_load_n(&seq, __ATOMIC_RELAXED);
        __atomic_store_n(&seq, s + 1, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        v = value;
        __atomic_store_n(&seq, s + 2, __ATOMIC_RELEASE);
    }
    // False when nothing was stored yet or it was being written right then.
    bool Load(T* out) const {
        uint32_t before = __atomic_load_n(&seq, __ATOMIC_ACQUIRE);
        if (before == 0 || (before & 1)) return false;
        *out = v;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        return __atomic_load_n(&seq, __ATOMIC_RELAXED) == before;
    }
};

struct DbcCount {
    const char* name;   // "ChrRaces"
    bool ok;            // could be read
    uint32_t records;
    int32_t maxId;
};

// Game state read at crash time from fixed Wow.exe 12340 addresses (see win_main.cpp).
struct GameState {
    bool valid;
    DbcCount dbc[MAX_DBC_COUNTS];
    unsigned dbcCount;
    // character creation (CCharacterCreation)
    bool createOk;
    uint32_t raceIndex;        // m_raceIndex
    uint32_t character;        // m_character
    uint32_t customizeFrame;   // m_charCustomizeFrame
    uint32_t raceCount;        // m_races.m_count
    int32_t races[MAX_CREATE_RACES];
    unsigned racesRead;
    unsigned prefsSet;         // non-null entries of m_charPreferences[22][2]
    // reputation (CGReputationInfo)
    bool repOk;
    uint32_t numHeaders;       // m_numHeaders (the array holds 64)
    unsigned nullHeaders;      // null pointers in m_headers[0..63] (all are set at start)
    // the last character list and create request
    bool haveList;
    CharList list;
    bool haveCreate;
    CharCreate create;
};

void FormatGameState(Buf& b, const GameState& g, uint32_t now);

// ---- what goes in a report ----

struct Module {
    uint32_t base;
    uint32_t size;
    bool mainExe;
    char name[64];    // file name
    char path[300];   // full path (UTF-8)
    char version[32]; // "3.3.5.12340", or empty
};

struct FileEntry {
    char name[96];    // "Data\\patch-A.MPQ"
    uint64_t size;
};

struct Registers {
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp, esp, eip, eflags;
};

enum CrashKind {
    KIND_EXCEPTION = 0,  // a CPU exception nothing caught
    KIND_FATAL = 1,      // the game's own fatal error (SErrDisplayError, not recoverable)
};

enum { MAX_FRAMES = 48, MAX_SCAN = 40, MAX_MODULES = 200, MAX_FILES = 160 };

struct CrashInfo {
    int kind;
    uint32_t code;          // exception code, or the game's error code for KIND_FATAL
    uint32_t address;       // where it happened
    bool hasAccess;
    uint32_t accessType;    // 0 read, 1 write, 8 execute
    uint32_t accessAddress;
    char message[600];      // the game's error text for KIND_FATAL
    uint32_t threadId;
    bool mainThread;
    bool hasRegisters;
    Registers regs;
    const char* stackMethod; // "StackWalk64", "frame pointers"
    uint32_t frames[MAX_FRAMES];
    unsigned frameCount;
    uint32_t scan[MAX_SCAN];  // return addresses found on the stack
    unsigned scanCount;
    uint32_t stackTop;        // esp the scan started from
    uint32_t tick;            // GetTickCount at the crash
    uint32_t startTick;       // GetTickCount when the DLL started
    char time[32];            // "2026-11-24 21:05:13"
    char exePath[300];
    uint32_t exeStamp;        // PE time stamp of the main exe
    bool buildOk;             // 3.3.5a 12340 seen at start
    char hooks[160];          // which hooks are on
    char dumpName[96];        // the .dmp file name, or empty
    char dumpError[96];       // why there is no dump
    const Module* modules;
    unsigned moduleCount;
    const FileEntry* files;
    unsigned fileCount;
    const GameState* game;    // null when it could not be read
};

// "0x0063A1B2 Wow.exe+0x23A1B2 NetClient::ProcessMessage+0x42"
void DescribeAddress(Buf& b, uint32_t address, const Module* modules, unsigned count);
const Module* FindModule(uint32_t address, const Module* modules, unsigned count);

// The whole report text.
void FormatReport(Buf& b, const CrashInfo& info,
                  const PacketEntry* in, unsigned inCount, uint32_t inTotal,
                  const PacketEntry* out, unsigned outCount, uint32_t outTotal,
                  const TextEntry* lua, unsigned luaCount, uint32_t luaTotal);

// True when the 5 to 7 bytes before address look like the end of a call instruction, so address is most likely
// a return address. code points at address - 7 (7 readable bytes).
bool LooksLikeReturn(const uint8_t* before7);

// True when the text has the crash test marker ("tc-crash-test"); kind gets the word after it, or "av".
bool FindTestMarker(const uint8_t* data, size_t size, char* kind, size_t kindSize);

}  // namespace tc
