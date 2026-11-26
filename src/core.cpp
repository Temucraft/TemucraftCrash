// TemucraftCrash: text building, name lookups and the report layout. No Windows calls, no heap.
#include "core.h"

namespace tc {

#include "../data/functions.inc"
#include "../data/opcodes.inc"

static const char kVersion[] = "1.3.1";

// ---- Buf ----

void Buf::ch(char c) {
    if (len + 1 < cap) {
        p[len++] = c;
        p[len] = 0;
    } else {
        full = true;
    }
}

void Buf::str(const char* s) {
    if (!s) s = "(null)";
    while (*s) ch(*s++);
}

void Buf::strn(const char* s, size_t n) {
    if (!s) return;
    for (size_t i = 0; i < n && s[i]; i++) ch(s[i]);
}

void Buf::hex(uint32_t v, int digits) {
    static const char d[] = "0123456789ABCDEF";
    // at least `digits` digits, more when the value needs them
    if (digits < 1) digits = 1;
    if (digits > 8) digits = 8;
    int need = 1;
    while (need < 8 && (v >> (need * 4))) need++;
    if (need > digits) digits = need;
    for (int i = digits - 1; i >= 0; i--) ch(d[(v >> (i * 4)) & 0xF]);
}

void Buf::dec(uint64_t v) {
    char t[24];
    int n = 0;
    do {
        t[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v && n < 24);
    while (n) ch(t[--n]);
}

void Buf::sdec(int64_t v) {
    if (v < 0) {
        ch('-');
        dec(static_cast<uint64_t>(-(v + 1)) + 1);
    } else {
        dec(static_cast<uint64_t>(v));
    }
}

void Buf::padTo(size_t column) {
    size_t start = len;
    while (start > 0 && p[start - 1] != '\n') start--;
    while (len - start < column && !full) ch(' ');
}

void Buf::seconds(uint32_t ms) {
    dec(ms / 1000);
    ch('.');
    uint32_t r = ms % 1000;
    ch(static_cast<char>('0' + r / 100));
    ch(static_cast<char>('0' + (r / 10) % 10));
    ch(static_cast<char>('0' + r % 10));
}

void Buf::size(uint64_t bytes) {
    if (bytes < 1024) {
        dec(bytes);
        str(bytes == 1 ? " byte" : " bytes");
        return;
    }
    const char* unit = " KB";
    uint64_t div = 1024;
    if (bytes >= 1024ull * 1024 * 1024) { unit = " GB"; div = 1024ull * 1024 * 1024; }
    else if (bytes >= 1024ull * 1024) { unit = " MB"; div = 1024ull * 1024; }
    uint64_t tenths = (bytes * 10 + div / 2) / div;
    dec(tenths / 10);
    if (tenths < 1000) {
        ch('.');
        dec(tenths % 10);
    }
    str(unit);
}

// ---- names ----

unsigned WowFunctionCount() { return kFuncCount; }

const char* WowFunction(uint32_t address, uint32_t* offset) {
    // past this distance from the last known start the address is most likely in a function the list doesn't have
    const uint32_t kMaxOffset = 0x2000;
    if (kFuncCount == 0 || address < kFuncStarts[0]) return nullptr;
    unsigned lo = 0, hi = kFuncCount;  // first start > address
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (kFuncStarts[mid] <= address) lo = mid + 1;
        else hi = mid;
    }
    unsigned i = lo - 1;
    uint32_t off = address - kFuncStarts[i];
    if (off > kMaxOffset) return nullptr;
    if (offset) *offset = off;
    return kFuncNames + kFuncNameAt[i];
}

const char* OpcodeName(uint32_t opcode) {
    if (opcode >= kOpcodeCount) return nullptr;
    const char* n = kOpcodeNames[opcode];
    return n[0] ? n : nullptr;
}

const char* Version() { return kVersion; }

const char* ExceptionName(uint32_t code) {
    switch (code) {
        case 0xC0000005: return "ACCESS_VIOLATION";
        case 0xC0000006: return "IN_PAGE_ERROR";
        case 0xC000001D: return "ILLEGAL_INSTRUCTION";
        case 0xC0000025: return "NONCONTINUABLE_EXCEPTION";
        case 0xC000008C: return "ARRAY_BOUNDS_EXCEEDED";
        case 0xC000008D: return "FLT_DENORMAL_OPERAND";
        case 0xC000008E: return "FLT_DIVIDE_BY_ZERO";
        case 0xC0000090: return "FLT_INVALID_OPERATION";
        case 0xC0000094: return "INT_DIVIDE_BY_ZERO";
        case 0xC0000095: return "INT_OVERFLOW";
        case 0xC0000096: return "PRIVILEGED_INSTRUCTION";
        case 0xC00000FD: return "STACK_OVERFLOW";
        case 0xC0000374: return "HEAP_CORRUPTION";
        case 0xC0000409: return "STACK_BUFFER_OVERRUN";
        case 0xC0000417: return "INVALID_CRUNTIME_PARAMETER";
        case 0x80000003: return "BREAKPOINT";
        case 0xE06D7363: return "C++ EXCEPTION";
        case 0x85100084: return "game fatal exception (ERROR #132)";
        case 0x85100086: return "game fatal condition (ERROR #134)";
        default: return nullptr;
    }
}

// ---- addresses ----

const Module* FindModule(uint32_t address, const Module* modules, unsigned count) {
    for (unsigned i = 0; i < count; i++)
        if (address >= modules[i].base && address - modules[i].base < modules[i].size) return &modules[i];
    return nullptr;
}

void DescribeAddress(Buf& b, uint32_t address, const Module* modules, unsigned count) {
    b.hex0x(address);
    const Module* m = FindModule(address, modules, count);
    if (!m) {
        b.str(" (not in a module)");
        return;
    }
    b.ch(' ');
    b.str(m->name);
    b.str("+0x");
    b.hex(address - m->base, 1);
    if (m->mainExe) {
        uint32_t off = 0;
        if (const char* name = WowFunction(address, &off)) {
            b.ch(' ');
            b.str(name);
            if (off) {
                b.str("+0x");
                b.hex(off, 1);
            }
        }
    }
}

bool LooksLikeReturn(const uint8_t* c) {
    // c[0..6] are the 7 bytes before the address
    if (c[2] == 0xE8) return true;                                  // call rel32
    if (c[5] == 0xFF && (c[6] & 0x38) == 0x10) return true;         // call reg / [reg]
    if (c[4] == 0xFF && (c[5] & 0x38) == 0x10) return true;         // call [reg+disp8]
    if (c[1] == 0xFF && (c[2] & 0x38) == 0x10) return true;         // call [reg+disp32] / [disp32]
    if (c[0] == 0xFF && (c[1] & 0x38) == 0x10) return true;         // call [reg*s+disp32] (SIB)
    if (c[3] == 0xFF && (c[4] & 0x38) == 0x10) return true;         // call [reg*s+disp8] (SIB)
    return false;
}

bool FindTestMarker(const uint8_t* data, size_t size, char* kind, size_t kindSize) {
    static const char m[] = "tc-crash-test";
    const size_t n = sizeof(m) - 1;
    if (!data || size < n || kindSize < 2) return false;
    for (size_t i = 0; i + n <= size; i++) {
        size_t j = 0;
        while (j < n && data[i + j] == static_cast<uint8_t>(m[j])) j++;
        if (j != n) continue;
        size_t at = i + n;
        while (at < size && data[at] == ' ') at++;
        size_t k = 0;
        while (at < size && k + 1 < kindSize && data[at] >= 'a' && data[at] <= 'z') kind[k++] = static_cast<char>(data[at++]);
        kind[k] = 0;
        if (k == 0) {
            kind[0] = 'a';
            kind[1] = kindSize > 2 ? 'v' : 0;
            if (kindSize > 2) kind[2] = 0;
        }
        return true;
    }
    return false;
}

// ---- the report ----

static void Packets(Buf& b, const char* title, const PacketEntry* e, unsigned count, uint32_t total, uint32_t now) {
    b.nl();
    b.str(title);
    b.str(" (newest first, ");
    b.dec(count);
    b.str(" shown, ");
    b.dec(total);
    b.line(" since the game started):");
    if (count == 0) b.line("  (none)");
    for (unsigned i = 0; i < count; i++) {
        b.str("  -");
        b.seconds(now - e[i].tick);
        b.str(" s  ");
        const char* name = OpcodeName(e[i].opcode);
        b.str(name ? name : "unknown opcode");
        b.str(" (0x");
        b.hex(e[i].opcode, 3);
        b.str(")");
        b.padTo(58);
        b.size(e[i].size);
        if (e[i].headLen) {
            b.padTo(72);
            for (unsigned j = 0; j < e[i].headLen && j < HEAD_BYTES; j++) {
                if (j) b.ch(' ');
                b.hex(e[i].head[j], 2);
            }
            uint32_t v;
            // the numbers that show a reputation list index past the stock 128 (CGReputationInfo::m_factionStandings)
            if (e[i].opcode == 0x122 && e[i].headLen >= 4) {  // SMSG_INITIALIZE_FACTIONS: count
                v = static_cast<uint32_t>(e[i].head[0]) | (static_cast<uint32_t>(e[i].head[1]) << 8) |
                    (static_cast<uint32_t>(e[i].head[2]) << 16) | (static_cast<uint32_t>(e[i].head[3]) << 24);
                b.str("  (count ");
                b.dec(v);
                b.str(v > 128 ? ", over the stock client's 128)" : ")");
            } else if (e[i].opcode == 0x124 && e[i].headLen >= 13) {  // SMSG_SET_FACTION_STANDING: first list index
                v = static_cast<uint32_t>(e[i].head[9]) | (static_cast<uint32_t>(e[i].head[10]) << 8) |
                    (static_cast<uint32_t>(e[i].head[11]) << 16) | (static_cast<uint32_t>(e[i].head[12]) << 24);
                b.str("  (first list index ");
                b.dec(v);
                b.str(v > 127 ? ", past the stock client's 127)" : ")");
            }
        }
        b.nl();
    }
}

// ---- characters and races ----

namespace {
struct Reader {
    const uint8_t* p;
    size_t n;
    size_t at;
    bool ok;
    bool Skip(size_t k) {
        if (!ok || k > n - at) return ok = false;
        at += k;
        return true;
    }
    uint8_t U8() {
        if (!ok || at >= n) {
            ok = false;
            return 0;
        }
        return p[at++];
    }
    // copies a 0-terminated string (cut to cap - 1 chars); fails when there is no 0 before the end
    bool Str(char* out, size_t cap) {
        size_t k = 0;
        while (ok) {
            if (at >= n) return ok = false;
            char c = static_cast<char>(p[at++]);
            if (!c) break;
            if (k + 1 < cap) out[k++] = (c == '\r' || c == '\n' || c == '\t') ? ' ' : c;
        }
        if (cap) out[k < cap ? k : cap - 1] = 0;
        return ok;
    }
};
}  // namespace

bool ParseCharEnum(const uint8_t* data, size_t size, CharList* out) {
    if (!out) return false;
    out->count = 0;
    out->parsed = 0;
    out->complete = false;
    if (!data) return false;
    Reader r = {data, size, 0, true};
    out->count = r.U8();
    if (!r.ok) return false;
    for (unsigned i = 0; i < out->count; i++) {
        CharSummary s;
        s.name[0] = 0;
        r.Skip(8);  // guid
        r.Str(s.name, sizeof(s.name));
        s.race = r.U8();
        s.cls = r.U8();
        s.gender = r.U8();
        s.skin = r.U8();
        s.face = r.U8();
        s.hairStyle = r.U8();
        s.hairColor = r.U8();
        s.facialHair = r.U8();
        s.level = r.U8();
        // zone, map, x, y, z, guild, flags, customize flags (8 x 4), first login (1), pet display, level, family
        // (3 x 4), then 23 equipment slots of display id (4), inventory type (1), enchant (4)
        r.Skip(32 + 1 + 12 + 23 * 9);
        if (!r.ok) return out->parsed > 0;
        if (out->parsed < MAX_ENUM_CHARS) out->c[out->parsed++] = s;
    }
    out->complete = r.at == size;
    return true;
}

bool ParseCharCreate(const uint8_t* data, size_t size, CharCreate* out) {
    if (!out) return false;
    out->valid = false;
    if (!data) return false;
    Reader r = {data, size, 0, true};
    CharSummary& s = out->c;
    s.name[0] = 0;
    r.Str(s.name, sizeof(s.name));
    s.race = r.U8();
    s.cls = r.U8();
    s.gender = r.U8();
    s.skin = r.U8();
    s.face = r.U8();
    s.hairStyle = r.U8();
    s.hairColor = r.U8();
    s.facialHair = r.U8();
    out->outfit = r.U8();
    s.level = 0;
    out->valid = r.ok;
    return r.ok;
}

static void CharLine(Buf& b, const CharSummary& s, bool showLevel) {
    b.str("    ");
    b.str(s.name);
    b.padTo(22);
    b.str("race ");
    b.dec(s.race);
    b.str(", class ");
    b.dec(s.cls);
    b.str(", gender ");
    b.dec(s.gender);
    b.str(", skin ");
    b.dec(s.skin);
    b.str(" face ");
    b.dec(s.face);
    b.str(" hair ");
    b.dec(s.hairStyle);
    b.ch('/');
    b.dec(s.hairColor);
    b.str(" facial ");
    b.dec(s.facialHair);
    if (showLevel) {
        b.str(", level ");
        b.dec(s.level);
    }
    if (s.race >= 32) b.str("  [race id 32 or more: 32-bit race masks can't hold it]");
    else if (s.race >= 22) b.str("  [race id 22 or more: past the stock client's 22-race arrays]");
    b.nl();
}

void FormatGameState(Buf& b, const GameState& g, uint32_t now) {
    b.nl();
    b.line("Races and characters (read from the game at the crash):");
    if (!g.valid) {
        b.line("  (not read)");
        return;
    }
    for (unsigned i = 0; i < g.dbcCount && i < MAX_DBC_COUNTS; i++) {
        const DbcCount& d = g.dbc[i];
        b.str("  ");
        b.str(d.name);
        b.str(".dbc");
        b.padTo(32);
        if (!d.ok) {
            b.line("(could not read)");
            continue;
        }
        b.dec(d.records);
        b.str(" records, highest id ");
        b.sdec(d.maxId);
        b.nl();
    }
    if (g.createOk) {
        b.str("  Character creation: ");
        b.dec(g.raceCount);
        b.str(" races, selected index ");
        if (g.raceIndex == 0xFFFFFFFFu) b.str("none");
        else b.dec(g.raceIndex);
        b.str(", model ");
        b.hex0x(g.character);
        b.str(", frame ");
        b.hex0x(g.customizeFrame);
        b.str(", saved choices ");
        b.dec(g.prefsSet);
        b.line(" of 44");
        if (g.racesRead) {
            b.str("    race ids:");
            bool past = false;
            for (unsigned i = 0; i < g.racesRead && i < MAX_CREATE_RACES; i++) {
                b.ch(' ');
                b.sdec(g.races[i]);
                if (g.races[i] >= 22) past = true;
            }
            b.nl();
            if (past) b.line("    NOTE: a race id of 22 or more is in the list; the stock client keeps choices per race in a 22-race array (CCharacterCreation::m_charPreferences, 0x00B6B0D0), so it writes over the values after it");
        }
        if (g.raceIndex != 0xFFFFFFFFu && g.raceCount && g.raceIndex >= g.raceCount)
            b.line("    NOTE: the selected race index is past the race list (overwritten?)");
        if (g.raceCount > 10)
            b.line("    NOTE: more than 10 races: GetAvailableRaces pushes 3 values per race without lua_checkstack (stock Lua gives a C function 20 slots)");
    }
    if (g.repOk) {
        b.str("  Reputation headers: ");
        b.dec(g.numHeaders);
        b.str(" in use of 64, ");
        b.dec(g.nullHeaders);
        b.line(" null");
        if (g.numHeaders > 64 || g.nullHeaders)
            b.line("    NOTE: the header list is broken; a reputation list index past 127 writes over it (CGReputationInfo::m_factionStandings[128], 0x00C22B70)");
    }
    if (g.haveList) {
        b.str("  Last character list (-");
        b.seconds(now - g.list.tick);
        b.str(" s): ");
        b.dec(g.list.count);
        b.str(" characters");
        if (!g.list.complete) b.str(", could not read it all");
        b.nl();
        for (unsigned i = 0; i < g.list.parsed && i < MAX_ENUM_CHARS; i++) CharLine(b, g.list.c[i], true);
    } else {
        b.line("  Last character list: (none seen)");
    }
    if (g.haveCreate && g.create.valid) {
        b.str("  Last character create request (-");
        b.seconds(now - g.create.tick);
        b.str(" s), outfit ");
        b.dec(g.create.outfit);
        b.line(":");
        CharLine(b, g.create.c, false);
    }
}

void FormatReport(Buf& b, const CrashInfo& c,
                  const PacketEntry* in, unsigned inCount, uint32_t inTotal,
                  const PacketEntry* out, unsigned outCount, uint32_t outTotal,
                  const TextEntry* lua, unsigned luaCount, uint32_t luaTotal) {
    b.line("Temucraft crash report");
    b.str("Made by TemucraftCrash ");
    b.str(kVersion);
    b.line(". Please send this file (and the .dmp file with the same name) to the server's admin.");
    b.nl();
    b.str("Time: ");
    b.line(c.time);
    b.str("Game ran for: ");
    b.seconds(c.tick - c.startTick);
    b.line(" s");
    b.str("Game: ");
    b.line(c.exePath);
    b.str("Client build: ");
    b.str(c.buildOk ? "12340 (3.3.5a)" : "not 3.3.5a 12340, so no hooks");
    b.str(", Wow.exe time stamp ");
    b.hex(c.exeStamp);
    b.nl();
    b.str("Hooks: ");
    b.line(c.hooks);
    b.str("Thread: ");
    b.dec(c.threadId);
    b.line(c.mainThread ? " (the game's main thread)" : "");
    b.nl();

    if (c.kind == KIND_FATAL) {
        b.str("What happened: the game stopped with its own fatal error, code ");
        b.hex0x(c.code);
        if (const char* n = ExceptionName(c.code)) {
            b.str(" (");
            b.str(n);
            b.str(")");
        }
        b.nl();
        b.str("Called from: ");
        DescribeAddress(b, c.address, c.modules, c.moduleCount);
        b.nl();
    } else {
        b.str("What happened: exception ");
        b.hex0x(c.code);
        const char* n = ExceptionName(c.code);
        b.str(" (");
        b.str(n ? n : "unknown");
        b.line(")");
        b.str("Crashed at: ");
        DescribeAddress(b, c.address, c.modules, c.moduleCount);
        b.nl();
        if (c.hasAccess) {
            b.str(c.accessType == 1 ? "It tried to write to " : c.accessType == 8 ? "It tried to run code at " : "It tried to read from ");
            b.hex0x(c.accessAddress);
            if (c.accessAddress < 0x10000) b.str(" (a null pointer)");
            b.nl();
        }
    }
    if (c.message[0]) {
        b.str("Game's message: ");
        b.line(c.message);
    }

    if (c.hasRegisters) {
        b.nl();
        b.line("Registers:");
        const struct { const char* name; uint32_t v; } r[] = {
            {"EAX", c.regs.eax}, {"EBX", c.regs.ebx}, {"ECX", c.regs.ecx}, {"EDX", c.regs.edx},
            {"ESI", c.regs.esi}, {"EDI", c.regs.edi}, {"EBP", c.regs.ebp}, {"ESP", c.regs.esp},
            {"EIP", c.regs.eip}, {"EFLAGS", c.regs.eflags},
        };
        for (const auto& x : r) {
            b.str("  ");
            b.str(x.name);
            b.padTo(10);
            b.hex(x.v);
            const Module* m = FindModule(x.v, c.modules, c.moduleCount);
            if (m && x.name[1] != 'F') {
                b.str("  ");
                DescribeAddress(b, x.v, c.modules, c.moduleCount);
            }
            b.nl();
        }
    }

    b.nl();
    b.str("Stack of the crashing thread (");
    b.str(c.stackMethod ? c.stackMethod : "none");
    b.line("):");
    if (c.frameCount == 0) b.line("  (could not walk the stack)");
    for (unsigned i = 0; i < c.frameCount; i++) {
        b.str("  #");
        b.dec(i);
        b.padTo(7);
        DescribeAddress(b, c.frames[i], c.modules, c.moduleCount);
        b.nl();
    }
    if (c.scanCount) {
        b.nl();
        b.str("Return addresses found on the stack from ESP ");
        b.hex0x(c.stackTop);
        b.line(" (helps when the walk above stops early):");
        for (unsigned i = 0; i < c.scanCount; i++) {
            b.str("  ");
            DescribeAddress(b, c.scan[i], c.modules, c.moduleCount);
            b.nl();
        }
    }

    Packets(b, "Last server packets received", in, inCount, inTotal, c.tick);
    Packets(b, "Last client packets sent", out, outCount, outTotal, c.tick);

    b.nl();
    b.str("Last Lua errors (newest first, ");
    b.dec(luaTotal);
    b.line(" since the game started):");
    if (luaCount == 0) b.line("  (none)");
    for (unsigned i = 0; i < luaCount; i++) {
        b.str("  -");
        b.seconds(c.tick - lua[i].tick);
        b.str(" s  ");
        b.line(lua[i].text);
    }

    if (c.game) FormatGameState(b, *c.game, c.tick);

    b.nl();
    b.str("Game files (");
    b.dec(c.fileCount);
    b.line("):");
    for (unsigned i = 0; i < c.fileCount; i++) {
        b.str("  ");
        b.str(c.files[i].name);
        b.padTo(40);
        b.size(c.files[i].size);
        b.nl();
    }

    b.nl();
    b.str("Loaded DLLs (");
    b.dec(c.moduleCount);
    b.line("):");
    for (unsigned i = 0; i < c.moduleCount; i++) {
        const Module& m = c.modules[i];
        b.str("  ");
        b.hex(m.base);
        b.ch('-');
        b.hex(m.base + m.size);
        b.ch(' ');
        b.str(m.path[0] ? m.path : m.name);
        if (m.version[0]) {
            b.str(" (");
            b.str(m.version);
            b.str(")");
        }
        b.nl();
    }

    b.nl();
    if (c.dumpName[0]) {
        b.str("Minidump: ");
        b.str(c.dumpName);
        b.line(" (open it in WinDbg or Visual Studio)");
    } else {
        b.str("Minidump: none (");
        b.str(c.dumpError[0] ? c.dumpError : "not made");
        b.line(")");
    }
    if (b.full) b.line("(the report was cut short)");
}

}  // namespace tc
