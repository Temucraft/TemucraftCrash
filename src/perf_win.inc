// ---------------------------------------------------------------------------------------------------------------
// performance monitor (debug mode, or TemucraftPerf.txt in the game folder): Logs\TemucraftPerf.log
//
// Included into win_main.cpp (uses its Install / SafeRead / FileExists). Nothing here is installed unless the monitor
// is on. The measuring (src/perf.cpp) runs on the frame thread only: the thread that calls GxScenePresent. Each
// timed function there costs two QueryPerformanceCounter calls and a table lookup; nothing is written per call.
// Lines are collected in memory and written to disk by a thread of our own about twice a second.

#include "perf.h"

using perf::Monitor;

static bool g_perf;
static wchar_t g_perfLog[MAX_PATH];
static Monitor g_mon;
static perf::Settings g_perfSet;
static perf::HandleNames<1024> g_handles;
static perf::LabelCache<2048> g_labels;
static CRITICAL_SECTION g_perfLock;
static char g_perfBuf[256 * 1024];
static size_t g_perfLen;
static uint32_t g_perfDropped;
static char g_perfWrite[256 * 1024];
static HANDLE g_perfThread;
static DWORD g_frameThread;
static int64_t g_qpcFreq;
static char g_perfHooks[512];
// files read on other threads (background loading): counted, not broken down
static volatile int64_t g_bgFileTicks;
static volatile uint32_t g_bgFileOps;

static inline DWORD CurThread() {
    DWORD id;
    __asm__ volatile("movl %%fs:0x24, %0" : "=r"(id));
    return id;
}

static inline int64_t Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static inline bool OnFrameThread() { return g_frameThread && CurThread() == g_frameThread; }

static void PerfAppendFile(const char* data, size_t size) {
    HANDLE h = CreateFileW(g_perfLog, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, data, static_cast<DWORD>(size), &written, nullptr);
    CloseHandle(h);
}

// the monitor's lines: "[hh:mm:ss.mmm] " in front, into the buffer (dropped, and counted, when it is full)
static void PerfEmit(void*, const char* text, size_t len) {
    char stamp[20];
    Buf b(stamp, sizeof(stamp));
    SYSTEMTIME t;
    GetLocalTime(&t);
    b.ch('[');
    b.ch(static_cast<char>('0' + t.wHour / 10)); b.ch(static_cast<char>('0' + t.wHour % 10)); b.ch(':');
    b.ch(static_cast<char>('0' + t.wMinute / 10)); b.ch(static_cast<char>('0' + t.wMinute % 10)); b.ch(':');
    b.ch(static_cast<char>('0' + t.wSecond / 10)); b.ch(static_cast<char>('0' + t.wSecond % 10)); b.ch('.');
    b.ch(static_cast<char>('0' + t.wMilliseconds / 100)); b.ch(static_cast<char>('0' + t.wMilliseconds / 10 % 10));
    b.ch(static_cast<char>('0' + t.wMilliseconds % 10)); b.str("] ");
    EnterCriticalSection(&g_perfLock);
    if (g_perfLen + b.len + len <= sizeof(g_perfBuf)) {
        memcpy(g_perfBuf + g_perfLen, stamp, b.len);
        memcpy(g_perfBuf + g_perfLen + b.len, text, len);
        g_perfLen += b.len + len;
    } else {
        g_perfDropped++;
    }
    LeaveCriticalSection(&g_perfLock);
}

static void PerfFlush() {
    size_t n;
    uint32_t dropped;
    EnterCriticalSection(&g_perfLock);
    n = g_perfLen;
    memcpy(g_perfWrite, g_perfBuf, n);
    g_perfLen = 0;
    dropped = g_perfDropped;
    g_perfDropped = 0;
    LeaveCriticalSection(&g_perfLock);
    if (n) PerfAppendFile(g_perfWrite, n);
    if (dropped) {
        char line[80];
        Buf b(line, sizeof(line));
        b.str("(");
        b.dec(dropped);
        b.str(" lines lost: the log buffer was full)\r\n");
        PerfAppendFile(line, b.len);
    }
}

static DWORD WINAPI PerfWriterMain(void*) {
    for (;;) {
        Sleep(500);
        PerfFlush();
    }
}

// ---- names of Lua handlers ----

// CSimpleFrame and its subclasses keep each script (FrameScript_Object::ScriptIx) at a fixed place in the object
// (Engine/Source/Frame/*.h); RunScript gets the address of one, so its distance from the object says which script.
static const char* ScriptName(uint32_t slot) {
    switch (slot) {
        case 0x00C: return "OnEvent";
        // CSimpleAnim
        case 0x03C: return "OnLoad(anim)";
        case 0x044: return "OnPlay(anim)";
        case 0x04C: return "OnPause(anim)";
        case 0x054: return "OnStop(anim)";
        case 0x05C: return "OnFinished(anim)";
        case 0x064: return "OnUpdate(anim)";
        // CSimpleAnimGroup
        case 0x050: return "OnLoad(animgroup)";
        case 0x058: return "OnPlay(animgroup)";
        case 0x060: return "OnPause(animgroup)";
        case 0x068: return "OnStop(animgroup)";
        case 0x070: return "OnFinished(animgroup)";
        case 0x078: return "OnUpdate(animgroup)";
        case 0x080: return "OnLoop(animgroup)";
        // CSimpleFrame
        case 0x134: return "OnLoad";
        case 0x13C: return "OnSizeChanged";
        case 0x144: return "OnUpdate";
        case 0x14C: return "OnShow";
        case 0x154: return "OnHide";
        case 0x15C: return "OnEnter";
        case 0x164: return "OnLeave";
        case 0x16C: return "OnMouseDown";
        case 0x174: return "OnMouseUp";
        case 0x17C: return "OnMouseWheel";
        case 0x184: return "OnDragStart";
        case 0x18C: return "OnDragStop";
        case 0x194: return "OnReceiveDrag";
        case 0x19C: return "OnChar";
        case 0x1A4: return "OnKeyDown";
        case 0x1AC: return "OnKeyUp";
        case 0x1B4: return "OnAttributeChanged";
        case 0x1BC: return "OnEnable";
        case 0x1C4: return "OnDisable";
        // subclasses
        case 0x2B4: return "OnHorizontalScroll";
        case 0x2B8: return "OnValueChanged";
        case 0x2BC: return "OnVerticalScroll";
        case 0x2C0: return "OnMinMaxChanged/OnColorSelect";
        case 0x2C4: return "OnScrollRangeChanged";
        case 0x2D8: return "PreClick";
        case 0x2E0: return "OnClick";
        case 0x2E8: return "PostClick";
        case 0x2F0: return "OnDoubleClick";
        case 0x33C: return "OnMessageScrollChanged";
        case 0x358: return "OnUpdateModel";
        case 0x360: return "OnAnimFinished";
        case 0x380: return "OnEnterPressed";
        case 0x388: return "OnEscapePressed";
        case 0x390: return "OnSpacePressed";
        case 0x398: return "OnTabPressed";
        case 0x3A0: return "OnTextChanged";
        case 0x3A8: return "OnTextSet";
        case 0x3B0: return "OnCursorChanged";
        case 0x3B4: return "OnMovieFinished";
        case 0x3B8: return "OnInputLanguageChanged";
        case 0x3BC: return "OnMovieShowSubtitle";
        case 0x3C0: return "OnEditFocusGained";
        case 0x3C4: return "OnMovieHideSubtitle";
        case 0x3C8: return "OnEditFocusLost";
        case 0x3D0: return "OnCharComposition";
        default: return nullptr;
    }
}

typedef const char*(__thiscall* GetName_t)(void*);

static inline bool InExe(uint32_t a, uint32_t lo, uint32_t hi) { return a >= lo && a < hi; }

// FrameScript_Object::GetName() (vtable slot 1, as FrameScript_Object::GetDisplayName 0x00482AA0 calls it), only
// when the vtable and the function are inside Wow.exe
// (careful: every read goes through SafeRead when the object pointer is not one the game just handed us)
static const char* ObjectName(void* object, bool careful) {
    if (!object) return nullptr;
    uint32_t vt = 0;
    if (careful) {
        if (!SafeRead(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(object)), &vt, 4)) return nullptr;
    } else {
        vt = *static_cast<uint32_t*>(object);
    }
    if (!InExe(vt, 0x009DF000, 0x00AB6000)) return nullptr;            // .rdata
    uint32_t fn = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(vt))[1];   // inside Wow.exe's .rdata: readable
    if (!InExe(fn, 0x00401000, 0x009DF000)) return nullptr;            // .text
    return reinterpret_cast<GetName_t>(fn)(object);
}

static void LuaLabel(char* out, size_t cap, void* object, uint32_t slot, const void* event) {
    Buf b(out, cap);
    const char* name = ObjectName(object, false);
    if (name && name[0]) {
        b.strn(name, 40);
    } else if (object && (slot == 0x0C || slot >= 0x134)) {
        // an unnamed frame: its parent's name (CScriptRegion::m_parent at +0x94)
        uint32_t parent = 0;
        SafeRead(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(object)) + 0x94, &parent, 4);
        const char* pn = ObjectName(reinterpret_cast<void*>(static_cast<uintptr_t>(parent)), true);
        b.str(pn && pn[0] ? pn : "?");
        b.str(".(unnamed)");
    } else {
        b.str(object ? "(unnamed)" : "(no frame)");
    }
    b.ch(':');
    if (event) {
        b.str("OnEvent ");
        // FrameScript_EventObject: TSHashObject key (HASHKEY_CONSTSTRI m_str) at +0x14
        const char* ev = *reinterpret_cast<const char* const*>(static_cast<const char*>(event) + 0x14);
        b.strn(ev ? ev : "?", 40);
    } else {
        const char* s = ScriptName(slot);
        if (s) {
            b.str(s);
        } else if (slot) {
            b.str("script+");
            b.hex0x(slot, 3);
        } else {
            b.str("script");
        }
    }
}

// copies the end of a path when it is long (the file name is the useful part)
static void PathLabel(char* out, size_t cap, const char* path) {
    if (!path) path = "?";
    size_t len = 0;
    while (len < 1024 && path[len]) len++;
    const char* from = path;
    if (len + 1 > cap) {
        from = path + len - (cap - 4);
        Buf b(out, cap);
        b.str("...");
        b.str(from);
        return;
    }
    Buf b(out, cap);
    b.str(from);
}

static void AddFile(const char* name, int64_t ticks) {
    bool isNew;
    perf::NameEntry* e = g_mon.FileName(perf::HashName(name ? name : "?", 260), &isNew);
    if (!e) {
        g_mon.files.AddLost(ticks);
        return;
    }
    if (isNew) PathLabel(e->label, sizeof(e->label), name);
    e->ticks += ticks;
    e->calls++;
}

static void AddAsset(const char* name, int64_t ticks) {
    bool isNew;
    perf::NameEntry* e = g_mon.AssetName(perf::HashName(name ? name : "?", 260), &isNew);
    if (!e) {
        g_mon.assets.AddLost(ticks);
        return;
    }
    if (isNew) PathLabel(e->label, sizeof(e->label), name);
    e->ticks += ticks;
    e->calls++;
}

// ---- the hooks ----

typedef void(__cdecl* Void_t)();
typedef void(__cdecl* ExecuteRef_t)(int, void*, int, const char*, void*);
typedef void(__thiscall* RunScript_t)(void*, const void*, int, const char*);
typedef void(__cdecl* ExecuteStr_t)(const char*, const char*, const char*);
typedef int(__stdcall* OpenEx_t)(void*, const char*, uint32_t, void**);
typedef int(__stdcall* SFileRead_t)(void*, void*, uint32_t, uint32_t*, void*, uint32_t);
typedef int(__stdcall* ReadWhole_t)(void*, const char*, void**, uint32_t*, uint32_t, uint32_t, void*);
typedef void*(__cdecl* TexCreate_t)(const char*, uint32_t, void*, uint32_t);
typedef void*(__cdecl* TexCache_t)(const char*, uint32_t, uint32_t, uint32_t);
typedef void*(__thiscall* CreateModel_t)(void*, const char*, uint32_t);
typedef void(__thiscall* This1_t)(void*, uint32_t);
typedef void(__thiscall* This2_t)(void*, uint32_t, uint32_t);
typedef int(__cdecl* LuaC_t)(void*);
typedef void(__cdecl* Screen_t)(const char*);
typedef int(__cdecl* Idle_t)(const void*, void*);
typedef double(__cdecl* lua_tonumber_t)(void*, int);

static const uint32_t kLuaToNumber = 0x0084E030;

static Void_t o_Present;
static ExecuteRef_t o_ExecuteRef;
static RunScript_t o_RunScript;
static ExecuteStr_t o_ExecuteStr;
static OpenEx_t o_OpenEx;
static SFileRead_t o_SFileRead;
static ReadWhole_t o_ReadWhole;
static TexCreate_t o_TexCreate;
static TexCache_t o_TexCache;
static CreateModel_t o_CreateModel;
static This1_t o_RenderPrep;
static This2_t o_CharInit;
static LuaC_t o_SetRace, o_SetSex, o_SetClass, o_Cycle;
static Void_t o_Randomize, o_Reset;
static Screen_t o_SetScreen;
static Idle_t o_GlueIdle;

// what RunScript just asked FrameScript_Execute to run (frame thread only)
static void* g_pendingObj;
static const void* g_pendingScript;

// GxScenePresent (0x00682A00): the end of every drawn frame, at the login screens and in the world
static void __cdecl h_Present() {
    DWORD tid = CurThread();
    if (!g_frameThread) g_frameThread = tid;
    if (tid != g_frameThread) {
        o_Present();
        return;
    }
    int tok = g_mon.Enter(perf::CAT_PRESENT, Now());
    o_Present();
    int64_t now = Now();
    g_mon.Leave(tok, now, nullptr);
    if (__atomic_load_n(&g_bgFileOps, __ATOMIC_RELAXED)) {
        g_mon.bgFileTicks += __atomic_exchange_n(&g_bgFileTicks, 0, __ATOMIC_RELAXED);
        g_mon.bgFileOps += __atomic_exchange_n(&g_bgFileOps, 0u, __ATOMIC_RELAXED);
    }
    g_mon.FrameEnd(now);
}

// FrameScript_Object::RunScript (0x0081A2C0): which script of the object is about to run
static void __fastcall h_RunScript(void* self, void*, const void* script, int argc, const char* taint) {
    if (OnFrameThread()) {
        g_pendingObj = self;
        g_pendingScript = script;
    }
    o_RunScript(self, script, argc, taint);
}

// FrameScript_Execute(function, object, argCount, taint, event) (0x00819EA0): every script handler and event
static void __cdecl h_ExecuteRef(int function, void* object, int argc, const char* taint, void* event) {
    if (!OnFrameThread()) {
        o_ExecuteRef(function, object, argc, taint, event);
        return;
    }
    uint32_t slot = 0;
    if (!event && object && object == g_pendingObj && g_pendingScript) {
        uintptr_t d = reinterpret_cast<uintptr_t>(g_pendingScript) - reinterpret_cast<uintptr_t>(object);
        if (d < 0x1000) slot = static_cast<uint32_t>(d);
    }
    g_pendingObj = nullptr;
    g_pendingScript = nullptr;
    uint64_t key = (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(object)) << 32) |
                   (event ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(event)) : slot);
    bool isNew;
    perf::NameEntry* e = g_mon.LuaName(key, &isNew);
    if (e && isNew) {
        const char* known = g_labels.Find(key);
        if (known) {
            memcpy(e->label, known, sizeof(e->label));
        } else {
            LuaLabel(e->label, sizeof(e->label), object, slot, event);
            g_labels.Put(key, e->label);
        }
    }
    int depth = g_mon.Depth();
    int tok = g_mon.Enter(perf::CAT_LUA, Now());
    o_ExecuteRef(function, object, argc, taint, event);
    int64_t d = g_mon.Leave(tok, Now(), nullptr);
    if (tok < 0) g_mon.Truncate(depth);
    if (e) {
        e->ticks += d;
        e->calls++;
    } else {
        g_mon.lua.AddLost(d);
    }
}

// FrameScript_Execute(source, fileName, taint) (0x00819210): Lua text (macros, /run, RunScript)
static void __cdecl h_ExecuteStr(const char* source, const char* file, const char* taint) {
    if (!OnFrameThread()) {
        o_ExecuteStr(source, file, taint);
        return;
    }
    const char* name = file ? file : "?";
    bool isNew;
    perf::NameEntry* e = g_mon.LuaName(perf::HashName(name, 200) | 1ull, &isNew);
    if (e && isNew) {
        Buf b(e->label, sizeof(e->label));
        b.str("text ");
        b.strn(name, 50);
    }
    int depth = g_mon.Depth();
    int tok = g_mon.Enter(perf::CAT_LUA, Now());
    o_ExecuteStr(source, file, taint);
    int64_t d = g_mon.Leave(tok, Now(), nullptr);
    if (tok < 0) g_mon.Truncate(depth);
    if (e) {
        e->ticks += d;
        e->calls++;
    } else {
        g_mon.lua.AddLost(d);
    }
}

static inline void Background(int64_t t0) {
    __atomic_fetch_add(&g_bgFileTicks, Now() - t0, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_bgFileOps, 1u, __ATOMIC_RELAXED);
}

// SFile::OpenEx(archive, name, flags, result) (0x00424B50)
static int __stdcall h_OpenEx(void* archive, const char* name, uint32_t flags, void** result) {
    if (!OnFrameThread()) {
        int64_t t0 = Now();
        int r = o_OpenEx(archive, name, flags, result);
        Background(t0);
        return r;
    }
    int tok = g_mon.Enter(perf::CAT_FILES, Now());
    int r = o_OpenEx(archive, name, flags, result);
    bool outer;
    int64_t d = g_mon.Leave(tok, Now(), &outer);
    if (r && result && *result) g_handles.Put(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(*result)), name);
    if (outer) AddFile(name, d);
    return r;
}

// SFileReadFile(file, buffer, count, read, overlapped, unused) (0x00422530)
static int __stdcall h_SFileRead(void* file, void* buffer, uint32_t count, uint32_t* read, void* overlapped, uint32_t unused) {
    if (!OnFrameThread()) {
        int64_t t0 = Now();
        int r = o_SFileRead(file, buffer, count, read, overlapped, unused);
        Background(t0);
        return r;
    }
    int tok = g_mon.Enter(perf::CAT_FILES, Now());
    int r = o_SFileRead(file, buffer, count, read, overlapped, unused);
    bool outer;
    int64_t d = g_mon.Leave(tok, Now(), &outer);
    if (outer) {
        const char* name = g_handles.Find(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(file)));
        AddFile(name ? name : "(file opened earlier)", d);
    }
    return r;
}

// SFile::ReadFile(archive, name, data, size, extra, flags, overlapped) (0x00424E80): a whole file at once
static int __stdcall h_ReadWhole(void* archive, const char* name, void** data, uint32_t* size, uint32_t extra,
                                 uint32_t flags, void* overlapped) {
    if (!OnFrameThread()) {
        int64_t t0 = Now();
        int r = o_ReadWhole(archive, name, data, size, extra, flags, overlapped);
        Background(t0);
        return r;
    }
    int tok = g_mon.Enter(perf::CAT_FILES, Now());
    int r = o_ReadWhole(archive, name, data, size, extra, flags, overlapped);
    bool outer;
    int64_t d = g_mon.Leave(tok, Now(), &outer);
    if (outer) AddFile(name, d);
    return r;
}

// TextureCreate(fileName, texFlags, status, flags) (0x004B9760)
static void* __cdecl h_TexCreate(const char* name, uint32_t texFlags, void* status, uint32_t flags) {
    if (!OnFrameThread()) return o_TexCreate(name, texFlags, status, flags);
    int tok = g_mon.Enter(perf::CAT_TEXTURES, Now());
    void* r = o_TexCreate(name, texFlags, status, flags);
    bool outer;
    int64_t d = g_mon.Leave(tok, Now(), &outer);
    if (outer) AddAsset(name, d);
    return r;
}

// TextureCacheCreateTexture(fileName) (0x004F3930): the character textures (cdecl: the extra arguments are only
// passed on, the game's function reads the first)
static void* __cdecl h_TexCache(const char* name, uint32_t a, uint32_t b, uint32_t c) {
    if (!OnFrameThread()) return o_TexCache(name, a, b, c);
    int tok = g_mon.Enter(perf::CAT_TEXTURES, Now());
    void* r = o_TexCache(name, a, b, c);
    bool outer;
    int64_t d = g_mon.Leave(tok, Now(), &outer);
    if (outer) AddAsset(name, d);
    return r;
}

// CM2Scene::CreateModel(fileName, flags) (0x0081F8F0)
static void* __fastcall h_CreateModel(void* self, void*, const char* name, uint32_t flags) {
    if (!OnFrameThread()) return o_CreateModel(self, name, flags);
    int tok = g_mon.Enter(perf::CAT_MODELS, Now());
    void* r = o_CreateModel(self, name, flags);
    bool outer;
    int64_t d = g_mon.Leave(tok, Now(), &outer);
    if (outer) AddAsset(name, d);
    return r;
}

// CCharacterComponent::RenderPrep(a) (0x004F1520) and CCharacterComponent::Init(a, b) (0x004F24D0)
static void __fastcall h_RenderPrep(void* self, void*, uint32_t a) {
    if (!OnFrameThread()) {
        o_RenderPrep(self, a);
        return;
    }
    int tok = g_mon.Enter(perf::CAT_CHARACTER, Now());
    o_RenderPrep(self, a);
    g_mon.Leave(tok, Now(), nullptr);
}

static void __fastcall h_CharInit(void* self, void*, uint32_t a, uint32_t b) {
    if (!OnFrameThread()) {
        o_CharInit(self, a, b);
        return;
    }
    int tok = g_mon.Enter(perf::CAT_CHARACTER, Now());
    o_CharInit(self, a, b);
    g_mon.Leave(tok, Now(), nullptr);
}

// the character create functions: "SetSelectedRace(3)" with the number arguments
static void CallLabel(char* out, size_t cap, const char* name, void* L, int args) {
    Buf b(out, cap);
    b.str(name);
    b.ch('(');
    for (int i = 1; L && i <= args; i++) {
        if (reinterpret_cast<lua_type_t>(kLuaType)(L, i) != 3 /* LUA_TNUMBER */) break;
        double v = reinterpret_cast<lua_tonumber_t>(kLuaToNumber)(L, i);
        if (i > 1) b.str(", ");
        if (v < 0) {
            b.ch('-');
            v = -v;
        }
        b.dec(static_cast<uint64_t>(v < 1e9 ? v : 1e9));
    }
    b.ch(')');
}

static int TimedLuaC(LuaC_t original, void* L, const char* name, int args) {
    if (!OnFrameThread()) return original(L);
    char label[64];
    CallLabel(label, sizeof(label), name, L, args);
    perf::CallMark m;
    g_mon.CallBegin(&m, Now());
    int r = original(L);
    g_mon.CallEnd(m, label, Now());
    return r;
}

static int __cdecl h_SetRace(void* L) { return TimedLuaC(o_SetRace, L, "SetSelectedRace", 1); }
static int __cdecl h_SetSex(void* L) { return TimedLuaC(o_SetSex, L, "SetSelectedSex", 1); }
static int __cdecl h_SetClass(void* L) { return TimedLuaC(o_SetClass, L, "SetSelectedClass", 1); }
static int __cdecl h_Cycle(void* L) { return TimedLuaC(o_Cycle, L, "CycleCharCustomization", 2); }

static void TimedVoid(Void_t original, const char* label) {
    if (!OnFrameThread()) {
        original();
        return;
    }
    perf::CallMark m;
    g_mon.CallBegin(&m, Now());
    original();
    g_mon.CallEnd(m, label, Now());
}

static void __cdecl h_Randomize() { TimedVoid(o_Randomize, "RandomizeCharCustomization()"); }
static void __cdecl h_Reset() { TimedVoid(o_Reset, "ResetCharCustomize()"); }

// CGlueMgr::SetScreen(name) (0x004D82A0): the login screens ("login", "charselect", "charcreate", ...)
static void __cdecl h_SetScreen(const char* name) {
    if (g_perf && (!g_frameThread || OnFrameThread())) {
        size_t len = 0;
        while (name && len < 40 && name[len]) len++;
        g_mon.Screen(name ? name : "?", len, Now());
    }
    o_SetScreen(name);
}

// CGlueMgr::Idle (0x004DAB40): runs every frame while the login screens are up
static int __cdecl h_GlueIdle(const void* data, void* param) {
    if (OnFrameThread()) g_mon.GlueIdle();
    return o_GlueIdle(data, param);
}

static InlineHook g_perfHooksList[] = {
    {0x00682A00, 6, {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C}, reinterpret_cast<void*>(h_Present), reinterpret_cast<void**>(&o_Present), false},
    {0x0081A2C0, 6, {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x10}, reinterpret_cast<void*>(h_RunScript), reinterpret_cast<void**>(&o_RunScript), false},
    {0x00819EA0, 6, {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x20}, reinterpret_cast<void*>(h_ExecuteRef), reinterpret_cast<void**>(&o_ExecuteRef), false},
    {0x00819210, 11, {0x55, 0x8B, 0xEC, 0x51, 0x83, 0x05, 0xA0, 0x13, 0xD4, 0x00, 0x01}, reinterpret_cast<void*>(h_ExecuteStr), reinterpret_cast<void**>(&o_ExecuteStr), false},
    {0x00424B50, 9, {0x55, 0x8B, 0xEC, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00}, reinterpret_cast<void*>(h_OpenEx), reinterpret_cast<void**>(&o_OpenEx), false},
    {0x00422530, 9, {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00}, reinterpret_cast<void*>(h_SFileRead), reinterpret_cast<void**>(&o_SFileRead), false},
    {0x00424E80, 6, {0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x1C}, reinterpret_cast<void*>(h_ReadWhole), reinterpret_cast<void**>(&o_ReadWhole), false},
    {0x004B9760, 9, {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x10, 0x01, 0x00, 0x00}, reinterpret_cast<void*>(h_TexCreate), reinterpret_cast<void**>(&o_TexCreate), false},
    {0x004F3930, 7, {0x55, 0x8B, 0xEC, 0x53, 0x8B, 0x5D, 0x08}, reinterpret_cast<void*>(h_TexCache), reinterpret_cast<void**>(&o_TexCache), false},
    {0x0081F8F0, 6, {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08}, reinterpret_cast<void*>(h_CreateModel), reinterpret_cast<void**>(&o_CreateModel), false},
    {0x004F1520, 6, {0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1}, reinterpret_cast<void*>(h_RenderPrep), reinterpret_cast<void**>(&o_RenderPrep), false},
    {0x004F24D0, 6, {0x55, 0x8B, 0xEC, 0x53, 0x8B, 0xD9}, reinterpret_cast<void*>(h_CharInit), reinterpret_cast<void**>(&o_CharInit), false},
    {0x004E2470, 6, {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C}, reinterpret_cast<void*>(h_SetRace), reinterpret_cast<void**>(&o_SetRace), false},
    {0x004E1A60, 7, {0x55, 0x8B, 0xEC, 0x56, 0x8B, 0x75, 0x08}, reinterpret_cast<void*>(h_SetSex), reinterpret_cast<void**>(&o_SetSex), false},
    {0x004E1AC0, 6, {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C}, reinterpret_cast<void*>(h_SetClass), reinterpret_cast<void**>(&o_SetClass), false},
    {0x004E0B50, 6, {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C}, reinterpret_cast<void*>(h_Cycle), reinterpret_cast<void**>(&o_Cycle), false},
    {0x004E17F0, 8, {0x8B, 0x0D, 0xA0, 0xB1, 0xB6, 0x00, 0x6A, 0x00}, reinterpret_cast<void*>(h_Randomize), reinterpret_cast<void**>(&o_Randomize), false},
    {0x004E1FD0, 8, {0x55, 0x8B, 0xEC, 0xA1, 0x84, 0xB1, 0xB6, 0x00}, reinterpret_cast<void*>(h_Reset), reinterpret_cast<void**>(&o_Reset), false},
    {0x004D82A0, 6, {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08}, reinterpret_cast<void*>(h_SetScreen), reinterpret_cast<void**>(&o_SetScreen), false},
    {0x004DAB40, 9, {0x55, 0x8B, 0xEC, 0x8B, 0x0D, 0x88, 0xDF, 0xC5, 0x00}, reinterpret_cast<void*>(h_GlueIdle), reinterpret_cast<void**>(&o_GlueIdle), false},
};
static const char* const kPerfHookNames[] = {
    "frame", "script slots", "Lua handlers", "Lua text", "file opens", "file reads", "whole files", "textures",
    "character textures", "models", "character RenderPrep", "character Init", "SetSelectedRace", "SetSelectedSex",
    "SetSelectedClass", "CycleCharCustomization", "RandomizeCharCustomization", "ResetCharCustomize", "glue screens",
    "glue idle"};

// ---- the Lua functions ----

static int __cdecl Lua_PerfMark(void* L) {
    if (!g_perf || !OnFrameThread()) return 0;
    size_t len = 0;
    const char* text = reinterpret_cast<lua_tolstring_t2>(0x0084E0E0)(L, 1, &len);
    g_mon.Mark(text ? text : "?", text ? len : 1, Now());
    return 0;
}

static int __cdecl Lua_PerfSpan(void* L) {
    if (!g_perf || !OnFrameThread()) return 0;
    size_t len = 0;
    const char* text = reinterpret_cast<lua_tolstring_t2>(0x0084E0E0)(L, 1, &len);
    g_mon.SpanToggle(text ? text : "?", text ? len : 1, Now());
    return 0;
}

// ---- start-up (called from Start, before the trampoline page is made read-only) ----

static void StartPerf() {
    bool marker = FileExists(L"TemucraftPerf.txt");
    if (!(g_debug || marker) || FileExists(L"TemucraftPerf-off.txt")) return;
    LARGE_INTEGER f;
    if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0) return;
    g_qpcFreq = f.QuadPart;

    wchar_t logs[MAX_PATH];
    lstrcpynW(logs, g_gameDir, MAX_PATH);
    if (lstrlenW(logs) + 40 >= MAX_PATH) return;
    lstrcatW(logs, L"\\Logs");
    CreateDirectoryW(logs, nullptr);
    lstrcpyW(g_perfLog, logs);
    lstrcatW(g_perfLog, L"\\TemucraftPerf.log");

    perf::DefaultSettings(&g_perfSet);
    if (marker) {
        wchar_t path[MAX_PATH * 2];
        lstrcpynW(path, g_gameDir, MAX_PATH);
        lstrcatW(path, L"\\TemucraftPerf.txt");
        HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            char text[2048];
            DWORD got = 0;
            if (ReadFile(h, text, sizeof(text), &got, nullptr)) perf::ParseSettings(text, got, &g_perfSet);
            CloseHandle(h);
        }
    }

    InitializeCriticalSection(&g_perfLock);
    g_handles.Init();
    g_labels.Init();
    g_mon.Init(g_qpcFreq, g_perfSet, PerfEmit, nullptr);

    // the measuring functions first (the frame hook last, so nothing is counted half-installed)
    const unsigned n = sizeof(g_perfHooksList) / sizeof(g_perfHooksList[0]);
    unsigned on = 0;
    for (unsigned i = 1; i < n; i++) on += Install(g_perfHooksList[i]);
    on += Install(g_perfHooksList[0]);
    g_perf = g_perfHooksList[0].on;

    Buf h(g_perfHooks, sizeof(g_perfHooks));
    h.dec(on);
    h.str(" of ");
    h.dec(n);
    h.str(" hooks on");
    bool first = true;
    for (unsigned i = 0; i < n; i++) {
        if (g_perfHooksList[i].on) continue;
        h.str(first ? ", OFF: " : ", ");
        first = false;
        h.str(kPerfHookNames[i]);
    }

    char head[900];
    Buf b(head, sizeof(head));
    b.nl();
    b.str("=== game started, TemucraftCrash ");
    b.str(Version());
    b.str(" performance log");
    if (!g_perf) b.str(" OFF (the frame hook could not go in)");
    b.str(": hitch ");
    perf::PutMs(b, g_perfSet.hitchMs);
    b.str(" ms, smooth below ");
    perf::PutMs(b, g_perfSet.settleMs);
    b.str(" ms, stats every ");
    b.dec(g_perfSet.windowSec);
    b.str(" s; ");
    b.str(g_perfHooks);
    b.nl();
    PerfAppendFile(head, b.len);

    if (g_perf) g_perfThread = CreateThread(nullptr, 64 * 1024, PerfWriterMain, nullptr, 0, nullptr);
}
