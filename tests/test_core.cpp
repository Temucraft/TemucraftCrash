// Tests for the parts of TemucraftCrash that don't need Windows. Built and run on Linux by build.sh:
//   g++ -O2 -pthread tests/test_core.cpp src/core.cpp -o test_core && ./test_core [--sample]
// --sample also prints a whole example report.
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "../src/core.h"

using namespace tc;

static int g_failed, g_passed;

#define CHECK(x)                                                         \
    do {                                                                 \
        if (x) g_passed++;                                               \
        else {                                                           \
            g_failed++;                                                  \
            printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #x);       \
        }                                                                \
    } while (0)

static bool Has(const char* text, const char* part) { return strstr(text, part) != nullptr; }

static void TestBuf() {
    char t[256];
    Buf b(t, sizeof(t));
    b.hex(0x631FE0);
    CHECK(strcmp(t, "00631FE0") == 0);
    Buf c(t, sizeof(t));
    c.hex0x(0xC0000005);
    c.ch(' ');
    c.hex(0x17D, 3);
    c.ch(' ');
    c.hex(0x1234, 1);
    c.ch(' ');
    c.dec(0);
    c.ch(' ');
    c.dec(18446744073709551615ull);
    c.ch(' ');
    c.sdec(-42);
    CHECK(strcmp(t, "0xC0000005 17D 1234 0 18446744073709551615 -42") == 0);

    Buf s(t, sizeof(t));
    s.seconds(0);
    s.ch(' ');
    s.seconds(1234);
    s.ch(' ');
    s.seconds(60005);
    CHECK(strcmp(t, "0.000 1.234 60.005") == 0);

    Buf z(t, sizeof(t));
    z.size(1); z.ch('|'); z.size(312); z.ch('|'); z.size(1536); z.ch('|'); z.size(1572864); z.ch('|'); z.size(3ull << 30);
    CHECK(strcmp(t, "1 byte|312 bytes|1.5 KB|1.5 MB|3.0 GB") == 0);

    Buf p(t, sizeof(t));
    p.str("ab");
    p.padTo(6);
    p.str("x\r\n12");
    p.padTo(4);
    p.ch('y');
    CHECK(strcmp(t, "ab    x\r\n12  y") == 0);

    // never writes past the end, always ends with 0, says when it was cut
    char small[8];
    memset(small, 'Z', sizeof(small));
    Buf o(small, 6);
    o.str("hello world");
    CHECK(o.full);
    CHECK(o.len == 5);
    CHECK(strcmp(small, "hello") == 0);
    CHECK(small[6] == 'Z' && small[7] == 'Z');
    Buf e(small, 1);
    e.str("x");
    CHECK(small[0] == 0 && e.full);
}

static void TestNames() {
    uint32_t off = 99;
    const char* n = WowFunction(0x00631FE0, &off);
    CHECK(n && strcmp(n, "NetClient::ProcessMessage") == 0 && off == 0);
    n = WowFunction(0x00631FEA, &off);
    CHECK(n && strcmp(n, "NetClient::ProcessMessage") == 0 && off == 0xA);
    n = WowFunction(0x00632B50, &off);
    CHECK(n && strcmp(n, "NetClient::Send") == 0);
    n = WowFunction(0x00819730, &off);
    CHECK(n && strstr(n, "FrameScript_HandleError"));
    n = WowFunction(0x00771D10, &off);
    CHECK(n && strstr(n, "SErrDisplayError"));
    n = WowFunction(0x0084E0E0, &off);
    CHECK(n && strstr(n, "lua_tolstring"));
    CHECK(WowFunction(0x00001000, &off) == nullptr);
    CHECK(WowFunction(0x7FFF0000, &off) == nullptr);  // far past the last function
    CHECK(WowFunctionCount() > 20000);

    CHECK(OpcodeName(0x17D) && strcmp(OpcodeName(0x17D), "SMSG_GOSSIP_MESSAGE") == 0);
    CHECK(OpcodeName(0x095) && strcmp(OpcodeName(0x095), "CMSG_MESSAGECHAT") == 0);
    CHECK(OpcodeName(0x04B) && strcmp(OpcodeName(0x04B), "CMSG_LOGOUT_REQUEST") == 0);
    CHECK(OpcodeName(0x000) == nullptr);
    CHECK(OpcodeName(0x521) == nullptr);
    CHECK(OpcodeName(0xFFFFFFFF) == nullptr);
    CHECK(ExceptionName(0xC0000005) && strcmp(ExceptionName(0xC0000005), "ACCESS_VIOLATION") == 0);
    CHECK(ExceptionName(0x12345678) == nullptr);
}

static PacketRing<64> g_ring;

static void TestRingSimple() {
    static PacketRing<8> r;  // zeroed like the DLL's globals
    PacketEntry out[16];
    CHECK(r.Snapshot(out, 16) == 0);
    r.Push(10, 1, 100);
    r.Push(20, 2, 200);
    unsigned n = r.Snapshot(out, 16);
    CHECK(n == 2 && out[0].opcode == 2 && out[1].opcode == 1 && out[0].tick == 20 && out[1].size == 100);
    for (unsigned i = 3; i <= 20; i++) r.Push(i * 10, i, i * 100);
    n = r.Snapshot(out, 16);
    CHECK(n == 8);
    bool order = true;
    for (unsigned i = 0; i < n; i++) order = order && out[i].opcode == 20 - i;
    CHECK(order);
    CHECK(r.Total() == 20);
    n = r.Snapshot(out, 3);
    CHECK(n == 3 && out[2].opcode == 18);

    static TextRing<4> t;
    TextEntry te[4];
    char longText[2000];
    memset(longText, 'a', sizeof(longText));
    t.Push(1, "first", 5);
    t.Push(2, longText, sizeof(longText));
    const char withZero[] = {'a', 0, 'b'};
    t.Push(3, withZero, 3);
    n = t.Snapshot(te, 4);
    CHECK(n == 3);
    CHECK(strcmp(te[0].text, "a b") == 0);
    CHECK(strlen(te[1].text) == sizeof(te[1].text) - 1);
    CHECK(strcmp(te[2].text, "first") == 0);
}

// many writers at once, a reader taking snapshots at the same time: every entry it sees must be whole
struct WriterArg { int id; };
static volatile int g_stop;
static int g_writersDone;

static void* Writer(void* p) {
    int id = static_cast<WriterArg*>(p)->id;
    for (uint32_t i = 0; i < 200000; i++) {
        uint32_t v = (static_cast<uint32_t>(id) << 24) | (i & 0xFFFFFF);
        g_ring.Push(v, v, v ^ 0xA5A5A5A5u);
    }
    __atomic_add_fetch(&g_writersDone, 1, __ATOMIC_RELEASE);
    return nullptr;
}

static void TestRingThreads() {
    pthread_t t[4];
    WriterArg a[4];
    for (int i = 0; i < 4; i++) {
        a[i].id = i + 1;
        pthread_create(&t[i], nullptr, Writer, &a[i]);
    }
    PacketEntry out[64];
    unsigned torn = 0, seen = 0;
    // read until every writer is done (a fixed number of reads could all happen before the writers start)
    for (int k = 0; k < 20000 || __atomic_load_n(&g_writersDone, __ATOMIC_ACQUIRE) < 4; k++) {
        unsigned n = g_ring.Snapshot(out, 64);
        for (unsigned i = 0; i < n; i++) {
            seen++;
            if (out[i].tick != out[i].opcode || out[i].size != (out[i].opcode ^ 0xA5A5A5A5u)) torn++;
        }
    }
    for (int i = 0; i < 4; i++) pthread_join(t[i], nullptr);
    CHECK(torn == 0);
    CHECK(seen > 0);
    CHECK(g_ring.Total() == 800000);
    unsigned n = g_ring.Snapshot(out, 64);
    CHECK(n == 64);
    printf("  ring: %u entries read while 4 threads wrote, %u torn\n", seen, torn);
}

static void TestMarkers() {
    // CMSG_MESSAGECHAT body: type, language, then the text
    uint8_t pkt[64] = {0x95, 0, 0, 0, 1, 0, 0, 0, 7, 0, 0, 0};
    const char* text = "tc-crash-test stack";
    memcpy(pkt + 12, text, strlen(text) + 1);
    char kind[16];
    CHECK(FindTestMarker(pkt, 12 + strlen(text) + 1, kind, sizeof(kind)) && strcmp(kind, "stack") == 0);
    const char* plain = "say tc-crash-test";
    CHECK(FindTestMarker(reinterpret_cast<const uint8_t*>(plain), strlen(plain), kind, sizeof(kind)) && strcmp(kind, "av") == 0);
    const char* other = "tc-crash-tes";
    CHECK(!FindTestMarker(reinterpret_cast<const uint8_t*>(other), strlen(other), kind, sizeof(kind)));
    CHECK(!FindTestMarker(nullptr, 100, kind, sizeof(kind)));
    const char* addon = "tc-crash-test\tfatal";  // SendAddonMessage puts a tab between prefix and text
    CHECK(FindTestMarker(reinterpret_cast<const uint8_t*>(addon), strlen(addon), kind, sizeof(kind)));
}

static void TestReturns() {
    uint8_t callRel[7] = {0x90, 0x90, 0xE8, 1, 2, 3, 4};
    CHECK(LooksLikeReturn(callRel));
    uint8_t callReg[7] = {0x90, 0x90, 0x90, 0x90, 0x90, 0xFF, 0xD0};  // call eax
    CHECK(LooksLikeReturn(callReg));
    uint8_t callMem[7] = {0x90, 0xFF, 0x15, 0x48, 0xF2, 0x9D, 0x00};  // call [0x9DF248]
    CHECK(LooksLikeReturn(callMem));
    uint8_t callDisp8[7] = {0x90, 0x90, 0x90, 0x90, 0xFF, 0x50, 0x4C};  // call [eax+0x4C]
    CHECK(LooksLikeReturn(callDisp8));
    uint8_t nothing[7] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    CHECK(!LooksLikeReturn(nothing));
}

static void MakeSample(char* text, size_t size, bool print) {
    static Module mods[3];
    memset(mods, 0, sizeof(mods));
    mods[0] = {0x00400000, 0x00A00000, true, "Wow.exe", "C:\\Games\\Temucraft\\Wow.exe", ""};
    mods[1] = {0x10000000, 0x00030000, false, "version.dll", "C:\\Games\\Temucraft\\version.dll", ""};
    mods[2] = {0x77500000, 0x00180000, false, "ntdll.dll", "C:\\Windows\\SYSTEM32\\ntdll.dll", ""};
    static FileEntry files[3] = {{"Data\\common.MPQ", 2952790016ull}, {"Data\\patch-A.MPQ", 31457280}, {"Data\\enUS\\patch-enUS-3.MPQ", 1048576}};

    static CrashInfo c;
    memset(&c, 0, sizeof(c));
    c.kind = KIND_EXCEPTION;
    c.code = 0xC0000005;
    c.address = 0x0058A57A;
    c.hasAccess = true;
    c.accessType = 0;
    c.accessAddress = 0x00000010;
    c.threadId = 4412;
    c.mainThread = true;
    c.hasRegisters = true;
    c.regs = {0x00000000, 0x0F3A2C10, 0x1C2D0040, 0x00000001, 0x0F3A2C10, 0x00000000, 0x0019F9A8, 0x0019F990, 0x0058A57A, 0x00010246};
    c.stackMethod = "StackWalk64";
    uint32_t frames[] = {0x0058A57A, 0x00632018, 0x006334F0 + 0x5A, 0x00401234, 0x7755A1B0};
    memcpy(c.frames, frames, sizeof(frames));
    c.frameCount = 5;
    c.scan[0] = 0x00632018;
    c.scan[1] = 0x0063355A;
    c.scanCount = 2;
    c.stackTop = 0x0019F990;
    c.startTick = 1000;
    c.tick = 1000 + 3723456;
    strcpy(c.time, "2026-11-24 21:05:13");
    strcpy(c.exePath, "C:\\Games\\Temucraft\\Wow.exe");
    c.exeStamp = 0x4C2452FE;
    c.buildOk = true;
    strcpy(c.hooks, "server packets on, client packets on, Lua errors on, fatal errors on, filter guard on");
    strcpy(c.dumpName, "Temucraft-2026-11-24-210513.dmp");
    c.modules = mods;
    c.moduleCount = 3;
    c.files = files;
    c.fileCount = 3;

    PacketEntry in[4] = {{c.tick - 12, 0x17D, 312, 0, {}}, {c.tick - 450, 0x0A9, 1180, 0, {}}, {c.tick - 2010, 0x1DD, 8, 0, {}},
                         {c.tick - 2500, 0x122, 645, 16, {0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}}};
    PacketEntry out[2] = {{c.tick - 30, 0x17B, 8, 0, {}}, {c.tick - 3200, 0x0B5, 26, 0, {}}};

    // the race and character section
    static GameState g;
    memset(&g, 0, sizeof(g));
    g.valid = true;
    g.dbc[0] = {"ChrRaces", true, 42, 213};
    g.dbc[1] = {"CharSections", false, 0, 0};
    g.dbcCount = 2;
    g.createOk = true;
    g.raceIndex = 3;
    g.character = 0x1F000000;
    g.customizeFrame = 0x1E000000;
    g.raceCount = 12;
    for (unsigned i = 0; i < 12; i++) g.races[i] = static_cast<int32_t>(i + 12);
    g.racesRead = 12;
    g.prefsSet = 2;
    g.repOk = true;
    g.numHeaders = 3;
    g.nullHeaders = 1;
    // a character list with one character, as the server sends it
    static uint8_t list[300];
    memset(list, 0, sizeof(list));
    size_t at = 0;
    list[at++] = 1;
    at += 8;
    memcpy(list + at, "Testname", 9);
    at += 9;
    const uint8_t look[9] = {24, 1, 0, 2, 3, 4, 5, 6, 80};
    memcpy(list + at, look, 9);
    at += 9 + 32 + 1 + 12 + 23 * 9;
    CHECK(ParseCharEnum(list, at, &g.list) && g.list.complete && g.list.parsed == 1 && g.list.c[0].race == 24 && g.list.c[0].level == 80);
    CharList shortList;
    CHECK(!ParseCharEnum(list, 20, &shortList) && shortList.parsed == 0);
    g.list.tick = c.tick - 60000;
    g.haveList = true;
    const uint8_t create[] = {'N', 'e', 'w', 0, 30, 2, 1, 0, 1, 2, 3, 4, 0};
    CHECK(ParseCharCreate(create, sizeof(create), &g.create) && g.create.c.race == 30 && g.create.c.cls == 2);
    CHECK(!ParseCharCreate(create, 6, &g.create));
    CHECK(ParseCharCreate(create, sizeof(create), &g.create));
    g.create.tick = c.tick - 5000;
    g.haveCreate = true;
    c.game = &g;
    TextEntry lua[1];
    lua[0].tick = c.tick - 65000;
    strcpy(lua[0].text, "Interface\\AddOns\\Foo\\Foo.lua:12: attempt to index global 'Bar' (a nil value)");

    Buf b(text, size);
    FormatReport(b, c, in, 4, 1543, out, 2, 210, lua, 1, 1);
    if (print) fputs(text, stdout);

    CHECK(Has(text, "Temucraft crash report"));
    CHECK(Has(text, "exception 0xC0000005 (ACCESS_VIOLATION)"));
    CHECK(Has(text, "Crashed at: 0x0058A57A Wow.exe+0x18A57A CGGossipInfo::SetGossip+0x2A"));
    CHECK(Has(text, "It tried to read from 0x00000010 (a null pointer)"));
    CHECK(Has(text, "#1   0x00632018 Wow.exe+0x232018 NetClient::ProcessMessage+0x38"));
    CHECK(Has(text, "SMSG_GOSSIP_MESSAGE (0x17D)"));
    CHECK(Has(text, "-0.012 s"));
    CHECK(Has(text, "CMSG_GOSSIP_HELLO (0x17B)"));
    CHECK(Has(text, "attempt to index global 'Bar'"));
    CHECK(Has(text, "Data\\patch-A.MPQ"));
    CHECK(Has(text, "C:\\Windows\\SYSTEM32\\ntdll.dll"));
    CHECK(Has(text, "ntdll.dll+0x5A1B0"));
    CHECK(Has(text, "Game ran for: 3723.456 s"));
    CHECK(Has(text, "Minidump: Temucraft-2026-11-24-210513.dmp"));
    CHECK(Has(text, "(count 129, over the stock client's 128)"));
    CHECK(Has(text, "ChrRaces.dbc"));
    CHECK(Has(text, "42 records, highest id 213"));
    CHECK(Has(text, "(could not read)"));
    CHECK(Has(text, "race ids: 12 13 14"));
    CHECK(Has(text, "m_charPreferences"));
    CHECK(Has(text, "more than 10 races"));
    CHECK(Has(text, "the header list is broken"));
    CHECK(Has(text, "Testname"));
    CHECK(Has(text, "race 24, class 1, gender 0"));
    CHECK(Has(text, "past the stock client's 22-race arrays"));
    CHECK(Has(text, "Last character create request"));
    CHECK(Has(text, "race 30, class 2, gender 1"));
    CHECK(!b.full);

    // a small buffer: cut short but still a proper string
    static char tiny[300];
    Buf t(tiny, sizeof(tiny));
    FormatReport(t, c, in, 4, 1543, out, 2, 210, lua, 1, 1);
    CHECK(t.full && strlen(tiny) == sizeof(tiny) - 1);

    // a fatal error from the game, not an exception
    c.kind = KIND_FATAL;
    c.code = 0x85100086;
    c.address = 0x0058A57A;
    c.hasAccess = false;
    strcpy(c.message, "Crash test (TemucraftCrash test:1)");
    Buf f(text, size);
    FormatReport(f, c, in, 0, 0, out, 0, 0, lua, 0, 0);
    CHECK(Has(text, "the game stopped with its own fatal error, code 0x85100086 (game fatal condition (ERROR #134))"));
    CHECK(Has(text, "Game's message: Crash test"));
    CHECK(Has(text, "(none)"));
}

int main(int argc, char** argv) {
    bool sample = argc > 1 && strcmp(argv[1], "--sample") == 0;
    TestBuf();
    TestNames();
    TestRingSimple();
    TestRingThreads();
    TestMarkers();
    TestReturns();
    static char text[64 * 1024];
    if (sample) printf("---- sample report ----\n");
    MakeSample(text, sizeof(text), sample);
    if (sample) printf("---- end of sample ----\n");
    printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
