// Tests for the performance monitor (src/perf.cpp). Built and run on Linux by build.sh:
//   g++ -O2 tests/test_perf.cpp src/perf.cpp src/core.cpp -o test_perf && ./test_perf [--sample]
// --sample prints the log lines of a made-up character create session.
#include <stdio.h>
#include <string.h>

#include <string>

#include "../src/perf.h"

using namespace tc;
using namespace tc::perf;

static int g_failed, g_passed;

#define CHECK(x)                                                         \
    do {                                                                 \
        if (x) g_passed++;                                               \
        else {                                                           \
            g_failed++;                                                  \
            printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #x);       \
        }                                                                \
    } while (0)

static std::string g_out;
static void Collect(void*, const char* text, size_t len) { g_out.append(text, len); }
static bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// 1 tick = 1 microsecond
static const int64_t F = 1000000;
static int64_t Ms(double ms) { return static_cast<int64_t>(ms * 1000); }

static Monitor g_m;

static Monitor& Fresh(double hitch = 50) {
    Settings s;
    DefaultSettings(&s);
    s.hitchMs = hitch;
    g_m.Init(F, s, Collect, nullptr);
    g_out.clear();
    return g_m;
}

static void TestSettings() {
    Settings s;
    DefaultSettings(&s);
    CHECK(s.hitchMs == 50 && s.settleMs == 33 && s.windowSec == 10);
    const char* t = "Performance log on.\r\nhitch = 80\r\n settle=20.5\nwindow=5\nnothing=3\nhitch=1\n";
    ParseSettings(t, strlen(t), &s);
    CHECK(s.hitchMs == 80);          // hitch=1 is below the 5 ms floor, so it stays 80
    CHECK(s.settleMs == 20.5);
    CHECK(s.windowSec == 5);
    ParseSettings("", 0, &s);
    CHECK(s.hitchMs == 80);
}

static void TestPutMs() {
    char t[64];
    Buf b(t, sizeof(t));
    PutMs(b, 16.666);
    b.ch(' ');
    PutMs(b, 0.04);
    b.ch(' ');
    PutMs(b, 1234.56);
    b.ch(' ');
    PutMs(b, -3);
    CHECK(strcmp(t, "16.7 0.0 1234.6 0.0") == 0);
}

static void TestHash() {
    CHECK(HashName("Character\\Orc\\Male\\OrcMale.m2", 200) == HashName("character/orc/male/ORCMALE.M2", 200));
    CHECK(HashName("a.blp", 200) != HashName("b.blp", 200));
    CHECK(HashName(nullptr, 10) == HashName("", 10));
}

static void TestNameTable() {
    static NameTable<16, 8> t;
    t.Init();
    bool isNew;
    for (int i = 0; i < 8; i++) {
        NameEntry* e = t.Get(100 + i, &isNew);
        CHECK(e && isNew);
        e->ticks = i * 10;
        e->calls = 1;
        snprintf(e->label, sizeof(e->label), "n%d", i);
    }
    CHECK(t.Get(200, &isNew) == nullptr);   // full
    NameEntry* again = t.Get(103, &isNew);
    CHECK(again && !isNew && again->ticks == 30);
    unsigned top[5];
    unsigned n = t.Top(5, top);
    CHECK(n == 5);
    CHECK(strcmp(t.e[top[0]].label, "n7") == 0 && strcmp(t.e[top[4]].label, "n3") == 0);
    t.Reset();
    CHECK(t.count == 0);
    NameEntry* fresh = t.Get(103, &isNew);
    CHECK(fresh && isNew && fresh->ticks == 0);
    // many resets: the generation number keeps old slots out
    for (int i = 0; i < 1000; i++) t.Reset();
    CHECK(t.Get(5, &isNew) && isNew);
}

static void TestHandles() {
    static HandleNames<16> h;
    h.Init();
    h.Put(0x1234, "Character\\Orc\\Male\\OrcMale.m2");
    h.Put(0x5678, "x.blp");
    CHECK(h.Find(0x1234) && strcmp(h.Find(0x1234), "Character\\Orc\\Male\\OrcMale.m2") == 0);
    CHECK(h.Find(0x9999) == nullptr);
    CHECK(h.Find(0) == nullptr);
    for (uint32_t i = 1; i < 200; i++) h.Put(i * 4096, "spam");   // no crash, old ones may go
    h.Put(0x5678, "y.blp");
    CHECK(strcmp(h.Find(0x5678), "y.blp") == 0);
}

static void TestLabelCache() {
    static LabelCache<8> c;
    c.Init();
    CHECK(c.Find(1) == nullptr);
    c.Put(1, "CharacterCreateFrame:OnUpdate");
    c.Put(2, "x");
    CHECK(c.Find(1) && strcmp(c.Find(1), "CharacterCreateFrame:OnUpdate") == 0);
    c.Put(1, "y");
    CHECK(strcmp(c.Find(1), "y") == 0);
    for (uint64_t k = 10; k < 100; k++) c.Put(k, "z");   // full: old ones go, nothing breaks
    CHECK(c.Find(99) != nullptr);
}

static void TestNesting() {
    Monitor& m = Fresh();
    int64_t t = 1000;
    m.FrameEnd(t);   // first frame: start
    // lua 0..30 ms, inside it files 5..25 ms, inside that a nested lua 10..12 (counted once in lua's inclusive time)
    int a = m.Enter(CAT_LUA, t);
    int b = m.Enter(CAT_FILES, t + Ms(5));
    int c = m.Enter(CAT_LUA, t + Ms(10));
    bool outer;
    m.Leave(c, t + Ms(12), &outer);
    CHECK(!outer);
    m.Leave(b, t + Ms(25), &outer);
    CHECK(outer);
    int64_t d = m.Leave(a, t + Ms(30), &outer);
    CHECK(outer && d == Ms(30));
    CHECK(m.inclFrame[CAT_LUA] == Ms(30));
    CHECK(m.inclFrame[CAT_FILES] == Ms(20));
    CHECK(m.selfFrame[CAT_FILES] == Ms(18));
    CHECK(m.selfFrame[CAT_LUA] == Ms(12));
    CHECK(m.countFrame[CAT_LUA] == 1);
    CHECK(m.Depth() == 0);

    // a Lua error jumps over two scopes: leaving the outer one drops them
    int x = m.Enter(CAT_LUA, t + Ms(31));
    m.Enter(CAT_CALL, t + Ms(32));
    m.Enter(CAT_FILES, t + Ms(33));
    m.Leave(x, t + Ms(40), &outer);
    CHECK(outer && m.Depth() == 0);
    CHECK(m.catDepth[CAT_CALL] == 0 && m.catDepth[CAT_FILES] == 0);
    // leaving a dropped scope does nothing
    CHECK(m.Leave(x + 1, t + Ms(41), nullptr) == 0);
    // Truncate
    int y = m.Enter(CAT_LUA, t + Ms(42));
    m.Enter(CAT_TEXTURES, t + Ms(43));
    m.Truncate(y);
    CHECK(m.Depth() == 0 && m.catDepth[CAT_LUA] == 0 && m.catDepth[CAT_TEXTURES] == 0);
    // a full stack
    for (int i = 0; i < MAX_DEPTH; i++) CHECK(m.Enter(CAT_LUA, t) >= 0);
    CHECK(m.Enter(CAT_LUA, t) == -1);
    m.Truncate(0);
}

static void TestHitch() {
    Monitor& m = Fresh(50);
    int64_t t = 5000;
    m.FrameEnd(t);
    // a calm frame
    t += Ms(16);
    m.FrameEnd(t);
    CHECK(!Has(g_out, "HITCH"));
    // a slow frame with a Lua handler that loads two files
    int64_t f0 = t;
    int l = m.Enter(CAT_LUA, f0 + Ms(1));
    bool isNew;
    NameEntry* e = m.LuaName(0xABCD0144ull, &isNew);
    CHECK(e && isNew);
    snprintf(e->label, sizeof(e->label), "CharacterCreateRaceButton3:OnClick");
    int f = m.Enter(CAT_FILES, f0 + Ms(2));
    m.Leave(f, f0 + Ms(42), nullptr);
    NameEntry* fe = m.FileName(HashName("Character\\Orc\\Male\\OrcMale.m2", 200), &isNew);
    snprintf(fe->label, sizeof(fe->label), "Character\\Orc\\Male\\OrcMale.m2");
    fe->ticks += Ms(40);
    fe->calls++;
    int f2 = m.Enter(CAT_FILES, f0 + Ms(42));
    m.Leave(f2, f0 + Ms(52), nullptr);
    NameEntry* fe2 = m.FileName(HashName("x.blp", 200), &isNew);
    snprintf(fe2->label, sizeof(fe2->label), "x.blp");
    fe2->ticks += Ms(10);
    fe2->calls++;
    int64_t d = m.Leave(l, f0 + Ms(61), nullptr);
    e->ticks += d;
    e->calls++;
    int p = m.Enter(CAT_PRESENT, f0 + Ms(70));
    m.Leave(p, f0 + Ms(80), nullptr);
    m.FrameEnd(f0 + Ms(80));
    CHECK(Has(g_out, "HITCH 80.0 ms (frame 2, world)"));
    CHECK(Has(g_out, "self ms: lua 10.0, files 50.0, present 10.0, other 10.0"));
    CHECK(Has(g_out, "lua 60.0 ms, 1 scripts: CharacterCreateRaceButton3:OnClick 60.0"));
    CHECK(Has(g_out, "files 50.0 ms, 2 opens/reads: Character\\Orc\\Male\\OrcMale.m2 40.0, x.blp 10.0"));
    // tables were reset for the next frame
    CHECK(m.lua.count == 0 && m.files.count == 0 && m.inclFrame[CAT_LUA] == 0);
    g_out.clear();
    m.FrameEnd(f0 + Ms(96));
    CHECK(g_out.empty());
}

static void TestStats() {
    Monitor& m = Fresh(50);
    int64_t t = 1;
    m.FrameEnd(t);
    // 10 s: 590 frames of 16 ms and one of 200 ms (about 10.6 s)
    m.GlueIdle();
    m.Screen("charcreate", 10, t);
    g_out.clear();
    for (int i = 0; i < 300; i++) {
        t += Ms(16);
        m.GlueIdle();
        m.FrameEnd(t);
    }
    t += Ms(200);
    m.FrameEnd(t);
    for (int i = 0; i < 400 && !Has(g_out, "stats"); i++) {
        t += Ms(16);
        m.GlueIdle();
        m.FrameEnd(t);
    }
    CHECK(Has(g_out, "HITCH 200.0 ms"));
    CHECK(Has(g_out, "(frame 301, charcreate)"));
    CHECK(Has(g_out, "stats 10.0 s: "));
    CHECK(Has(g_out, "frame ms p50 16.0, p95 16.0, max 200.0"));
    CHECK(Has(g_out, " | 1 hitch | "));
    CHECK(Has(g_out, "| charcreate\r\n"));
    // the worst second has the 200 ms frame in it: fewer than 60 fps
    size_t at = g_out.find(" avg, ");
    CHECK(at != std::string::npos);
    if (at != std::string::npos) {
        double worst = atof(g_out.c_str() + at + 6);
        CHECK(worst > 40 && worst < 60);
    }
    // a window with no glue idle is "world"; file work on other threads is in the line
    g_out.clear();
    m.bgFileOps += 12;
    m.bgFileTicks += Ms(30.2);
    for (int i = 0; i < 700 && !Has(g_out, "stats"); i++) {
        t += Ms(16);
        m.FrameEnd(t);
    }
    CHECK(Has(g_out, "| world\r\n"));
    CHECK(Has(g_out, " | other threads: 12 file ops 30.2 ms | "));
    CHECK(m.bgFileOps == 0);
}

static void TestCallAndWatch() {
    Monitor& m = Fresh(50);
    int64_t t = 100;
    m.FrameEnd(t);
    t += Ms(16);
    m.FrameEnd(t);
    g_out.clear();
    // Lua OnClick -> SetSelectedRace(3) which builds the character (files + character)
    int l = m.Enter(CAT_LUA, t + Ms(1));
    CallMark cm;
    m.CallBegin(&cm, t + Ms(2));
    int ch = m.Enter(CAT_CHARACTER, t + Ms(3));
    int f = m.Enter(CAT_FILES, t + Ms(4));
    m.Leave(f, t + Ms(84), nullptr);
    m.Leave(ch, t + Ms(100), nullptr);
    m.CallEnd(cm, "SetSelectedRace(3)", t + Ms(102));
    m.Mark("race click", 10, t + Ms(103));
    m.Leave(l, t + Ms(104), nullptr);
    CHECK(Has(g_out, "call SetSelectedRace(3) 100.0 ms: files 80.0, character 17.0, own code 3.0"));
    CHECK(Has(g_out, "mark race click (frame 1)"));
    m.FrameEnd(t + Ms(110));
    CHECK(Has(g_out, "during: SetSelectedRace(3) 100.0 ms"));
    t += Ms(110);
    double next[] = {60, 40, 16, 17, 16};
    for (double ms : next) {
        t += Ms(ms);
        m.FrameEnd(t);
    }
    CHECK(Has(g_out, "after SetSelectedRace(3) + mark race click: next frame +108.0 ms, smooth again +208.0 ms (6 frames): 108.0* 60.0* 40.0* 16.0 17.0 16.0\r\n"));
    // a watch that never calms down
    g_out.clear();
    m.Mark("busy", 4, t);
    for (int i = 0; i < 45; i++) {
        t += Ms(40);
        m.FrameEnd(t);
    }
    CHECK(Has(g_out, "after mark busy: next frame +40.0 ms, still not smooth after 1600.0 ms (40 frames)"));
    // a new interaction after a frame cuts the old watch short
    g_out.clear();
    m.Mark("one", 3, t);
    t += Ms(40);
    m.FrameEnd(t);
    m.Mark("two", 3, t);
    CHECK(Has(g_out, "after mark one: next frame +40.0 ms, cut short by the next one after 40.0 ms (1 frame): 40.0*"));
}

static void TestSpans() {
    Monitor& m = Fresh(50);
    int64_t t = 100;
    m.FrameEnd(t);
    m.SpanToggle("race click", 10, t + Ms(1));
    t += Ms(16);
    m.FrameEnd(t);
    t += Ms(70);
    m.FrameEnd(t);
    m.SpanToggle("race click", 10, t + Ms(5));
    CHECK(Has(g_out, "span race click: 90.0 ms, 2 frames ended, 1 hitches"));
    // many spans: the oldest is dropped, nothing breaks
    for (int i = 0; i < 20; i++) {
        char n[16];
        snprintf(n, sizeof(n), "s%d", i);
        m.SpanToggle(n, strlen(n), t + i);
    }
    g_out.clear();
    m.SpanToggle("s19", 3, t + Ms(10));
    CHECK(Has(g_out, "span s19: "));
    // labels are cut and cleaned
    m.SpanToggle("a\nb", 3, t);
    m.SpanToggle("a b", 3, t + Ms(1));
    CHECK(Has(g_out, "span a b: 1.0 ms"));
}

static void TestOpenAtFrameEnd() {
    // a frame drawn from inside a scope (loading screen): the time so far counts in this frame, the rest in the next
    Monitor& m = Fresh(50);
    int64_t t = 100;
    m.FrameEnd(t);
    int l = m.Enter(CAT_FILES, t + Ms(10));
    m.FrameEnd(t + Ms(70));
    CHECK(Has(g_out, "self ms: files 60.0, other 10.0"));
    g_out.clear();
    m.Leave(l, t + Ms(90), nullptr);
    m.FrameEnd(t + Ms(150));
    CHECK(Has(g_out, "self ms: files 20.0, other 60.0"));
}

static void Sample() {
    Monitor& m = Fresh(50);
    g_out.clear();
    int64_t t = 1;
    m.FrameEnd(t);
    m.Screen("charcreate", 10, t);
    for (int i = 0; i < 30; i++) {
        t += Ms(16.7);
        m.GlueIdle();
        m.FrameEnd(t);
    }
    // the click
    int l = m.Enter(CAT_LUA, t + Ms(1));
    bool isNew;
    CallMark cm;
    m.CallBegin(&cm, t + Ms(2));
    int ch = m.Enter(CAT_CHARACTER, t + Ms(3));
    const char* names[] = {"Character\\Orc\\Male\\OrcMale.m2", "Character\\Orc\\Male\\OrcMale00.skin",
                           "Character\\Orc\\Male\\OrcMaleSkin00_00.blp", "Character\\Orc\\Hair00_00.blp"};
    double took[] = {35, 8, 22, 6};
    int64_t at = t + Ms(4);
    for (int i = 0; i < 4; i++) {
        int f = m.Enter(CAT_FILES, at);
        at += Ms(took[i]);
        m.Leave(f, at, nullptr);
        NameEntry* e = m.FileName(HashName(names[i], 200), &isNew);
        snprintf(e->label, sizeof(e->label), "%s", names[i]);
        e->ticks += Ms(took[i]);
        e->calls++;
    }
    int x = m.Enter(CAT_TEXTURES, at);
    at += Ms(9);
    m.Leave(x, at, nullptr);
    NameEntry* a = m.AssetName(1, &isNew);
    if (a) {
        snprintf(a->label, sizeof(a->label), "Character\\Orc\\Male\\OrcMaleSkin00_00.blp");
        a->ticks += Ms(9);
        a->calls++;
    }
    m.Leave(ch, at + Ms(12), nullptr);
    m.CallEnd(cm, "SetSelectedRace(2)", at + Ms(13));
    int64_t d = m.Leave(l, at + Ms(15), nullptr);
    NameEntry* le = m.LuaName(77, &isNew);
    snprintf(le->label, sizeof(le->label), "CharacterCreateRaceButton2:OnClick");
    le->ticks += d;
    le->calls++;
    t = at + Ms(30);
    m.GlueIdle();
    m.FrameEnd(t);
    double after[] = {48, 21, 16.7, 16.7, 16.7};
    for (double ms : after) {
        t += Ms(ms);
        m.GlueIdle();
        m.FrameEnd(t);
    }
    while (!Has(g_out, "stats")) {
        t += Ms(16.7);
        m.GlueIdle();
        m.FrameEnd(t);
    }
    printf("%s", g_out.c_str());
}

int main(int argc, char** argv) {
    TestSettings();
    TestPutMs();
    TestHash();
    TestNameTable();
    TestHandles();
    TestLabelCache();
    TestNesting();
    TestHitch();
    TestStats();
    TestCallAndWatch();
    TestSpans();
    TestOpenAtFrameEnd();
    if (argc > 1 && strcmp(argv[1], "--sample") == 0) Sample();
    printf("perf tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
