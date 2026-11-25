// TemucraftCrash: a crash report helper for the Temucraft 3.3.5a (12340) client.
//
// It is named version.dll and sits next to Wow.exe. Wow.exe imports version.dll, so Windows loads this file first;
// every version.dll function is passed on to Windows' own version.dll (see proxy.s).
//
// When the game crashes it writes Errors\Temucraft-<date>-<time>.txt and a .dmp next to it, before the game's own
// crash report (which still runs as before).
//
// Addresses are for Wow.exe 3.3.5a build 12340 and were checked against the wowdecomp source (// OFFSET: tags) and
// the bytes of the Temucraft Wow.exe. Each hook checks the bytes before it changes anything; if they differ, that
// hook stays off. If anything at start-up fails, the DLL only passes version.dll calls on and does nothing else.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <string.h>

#include "core.h"

using namespace tc;

// ---------------------------------------------------------------------------------------------------------------
// version.dll pass-through (used by proxy.s)

static const char* const kVersionNames[17] = {
    "GetFileVersionInfoA", "GetFileVersionInfoByHandle", "GetFileVersionInfoExA", "GetFileVersionInfoExW",
    "GetFileVersionInfoSizeA", "GetFileVersionInfoSizeExA", "GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW",
    "GetFileVersionInfoW", "VerFindFileA", "VerFindFileW", "VerInstallFileA", "VerInstallFileW", "VerLanguageNameA",
    "VerLanguageNameW", "VerQueryValueA", "VerQueryValueW"};

extern "C" {
void* g_versionReal[17];
void* __cdecl ResolveVersion(int index);
}

extern "C" void* __cdecl ResolveVersion(int index) {
    static HMODULE real;
    if (!real) {
        wchar_t path[MAX_PATH + 16];
        UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return nullptr;
        lstrcatW(path, L"\\version.dll");
        HMODULE h = LoadLibraryW(path);
        if (!h) return nullptr;
        for (int i = 0; i < 17; i++) {
            void* f = reinterpret_cast<void*>(GetProcAddress(h, kVersionNames[i]));
            __atomic_store_n(&g_versionReal[i], f, __ATOMIC_RELEASE);
        }
        real = h;
    }
    if (index < 0 || index >= 17) return nullptr;
    return __atomic_load_n(&g_versionReal[index], __ATOMIC_ACQUIRE);
}

// ---------------------------------------------------------------------------------------------------------------
// Wow.exe 3.3.5a (12340)

// PE time stamp of the 12340 Wow.exe (the Temucraft one, with its patches, still has it)
static const DWORD kWowTimeStamp = 0x4C2452FE;

// NetClient::ProcessMessage(this, timeReceived, CDataStore* msg, a4): every server packet goes through here.
static const uint32_t kProcessMessage = 0x00631FE0;
// NetClient::Send(this, CDataStore* msg): every client packet (ClientServices::SendPacket and Send2 both end here).
static const uint32_t kNetSend = 0x00632B50;
// FrameScript_HandleError(lua_State*): the game's Lua error function, message on the top of the Lua stack.
static const uint32_t kLuaHandleError = 0x00819730;
// SErrDisplayError(code, file, line, text, recoverable, exitcode, unknown): the game's error box (stdcall).
static const uint32_t kSErrDisplayError = 0x00771D10;
// lua_type(L, idx) and lua_tolstring(L, idx, len), both cdecl.
static const uint32_t kLuaType = 0x0084DEB0;
static const uint32_t kLuaToLString = 0x0084E0E0;

static const uint32_t CMSG_MESSAGECHAT = 0x095;
static const uint32_t SMSG_CHAR_ENUM = 0x03B;
static const uint32_t CMSG_CHAR_CREATE = 0x036;

// Game data read at crash time (only read, never written; each read goes through SafeRead).
// WowClientDB<T> (WoW/Source/DB/WowClientDB.h): m_numRecords at +0x08, m_maxID at +0x0C. The bases are the
// // STATIC: tags of WoW/Source/DB/DBClient/DBClient.cpp; the +0x08/+0x0C reads were seen in Wow.exe's own code
// (ChrRaces 0x00AD3434 in Script_GetAvailableRaces 0x004E1880, TalentTab 0x00AD4B88 in BuildTabInfo 0x005C64D0).
static const struct { const char* name; uint32_t base; } kDbcs[] = {
    {"ChrRaces", 0x00AD3428},     {"ChrClasses", 0x00AD3404}, {"CharBaseInfo", 0x00AD32E4},
    {"CharSections", 0x00AD332C}, {"Faction", 0x00AD3860},    {"FactionTemplate", 0x00AD3884},
    {"EmotesTextSound", 0x00AD37AC}, {"TalentTab", 0x00AD4B80},
};
// CCharacterCreation (WoW/Source/Glue/CharacterCreation.cpp)
static const uint32_t kCharPreferences = 0x00B6B0D0;  // CharacterPreferences* [22][2]
static const uint32_t kRaceIndex = 0x00B6B180;
static const uint32_t kCustomizeFrame = 0x00B6B184;
static const uint32_t kCharacter = 0x00B6B1A0;
static const uint32_t kRacesCount = 0x00B6B1D0;       // m_races (TSGrowableArray<int32_t> at 0x00B6B1CC): m_count
static const uint32_t kRacesData = 0x00B6B1D4;        // m_data
// CGReputationInfo (WoW/Source/Ui/ReputationInfo.cpp)
static const uint32_t kRepHeaders = 0x00C23370;       // FactionHeader* [64]
static const uint32_t kRepNumHeaders = 0x00C23470;

// CDataStore (Engine/Source/Base/CDataStore.h)
struct DataStore {
    void* vtable;
    uint8_t* data;   // 0x04
    uint32_t base;   // 0x08
    uint32_t alloc;  // 0x0C
    uint32_t size;   // 0x10
    uint32_t read;   // 0x14
};

// ---------------------------------------------------------------------------------------------------------------
// state

static HMODULE g_self;
static bool g_active;
static bool g_testMode;
static DWORD g_startTick;
static DWORD g_mainThreadId;
static DWORD g_exeStamp;
static bool g_buildOk;
static wchar_t g_gameDir[MAX_PATH];
static wchar_t g_exePath[MAX_PATH];
static char g_hooks[256];

static PacketRing<64> g_in;
static PacketRing<32> g_out;
static TextRing<8> g_lua;
static Latest<CharList> g_charList;
static Latest<CharCreate> g_charCreate;

// ---------------------------------------------------------------------------------------------------------------
// small helpers

static bool SafeRead(uint32_t address, void* out, size_t size) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(address)), out, size, &got) &&
           got == size;
}

// Copies a C string that might point at bad memory. Reads at most one page at a time.
static void SafeString(const char* src, char* dst, size_t cap) {
    if (!cap) return;
    dst[0] = 0;
    if (!src) return;
    size_t n = 0;
    uintptr_t at = reinterpret_cast<uintptr_t>(src);
    while (n + 1 < cap) {
        size_t toPage = 0x1000 - (at & 0xFFF);
        size_t want = cap - 1 - n;
        if (want > toPage) want = toPage;
        char tmp[256];
        if (want > sizeof(tmp)) want = sizeof(tmp);
        if (!SafeRead(static_cast<uint32_t>(at), tmp, want)) break;
        for (size_t i = 0; i < want; i++) {
            if (!tmp[i]) {
                dst[n] = 0;
                return;
            }
            char c = tmp[i];
            dst[n++] = (c == '\r' || c == '\n' || c == '\t') ? ' ' : c;
        }
        at += want;
    }
    dst[n] = 0;
}

static void ToUtf8(const wchar_t* w, char* out, int cap) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, nullptr, nullptr);
    if (n <= 0) out[0] = 0;
    out[cap - 1] = 0;
}

static bool FileExists(const wchar_t* name) {
    wchar_t path[MAX_PATH * 2];
    lstrcpynW(path, g_gameDir, MAX_PATH);
    lstrcatW(path, L"\\");
    lstrcatW(path, name);
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static bool IsFatalCode(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        case 0xC0000374:  // heap corruption
        case 0xC0000409:  // stack buffer overrun
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// inline hooks

struct InlineHook {
    uint32_t address;
    uint8_t length;           // bytes moved to the trampoline (whole instructions, none of them relative)
    uint8_t expect[12];
    void* detour;
    void** original;
    bool on;
};

static uint8_t* g_trampolines;
static size_t g_trampolineUsed;

static bool Install(InlineHook& h) {
    uint8_t* target = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(h.address));
    uint8_t now[12];
    if (h.length < 5 || h.length > sizeof(now)) return false;
    if (!SafeRead(h.address, now, h.length) || memcmp(now, h.expect, h.length) != 0) return false;
    if (!g_trampolines || g_trampolineUsed + 32 > 4096) return false;

    uint8_t* t = g_trampolines + g_trampolineUsed;
    memcpy(t, now, h.length);
    t[h.length] = 0xE9;
    int32_t back = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target + h.length) - reinterpret_cast<uintptr_t>(t + h.length + 5));
    memcpy(t + h.length + 1, &back, 4);
    g_trampolineUsed += 32;

    DWORD old = 0;
    if (!VirtualProtect(target, h.length, PAGE_EXECUTE_READWRITE, &old)) return false;
    uint8_t patch[12];
    patch[0] = 0xE9;
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(h.detour) - reinterpret_cast<uintptr_t>(target + 5));
    memcpy(patch + 1, &rel, 4);
    for (int i = 5; i < h.length; i++) patch[i] = 0x90;
    *h.original = t;  // set before the jump goes in
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    memcpy(target, patch, h.length);
    DWORD ignored;
    VirtualProtect(target, h.length, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, h.length);
    h.on = true;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// the crash test (only when TemucraftCrash-test.txt is in the game folder)

__attribute__((noinline)) static int TestStackOverflow(volatile int depth) {
    volatile char pad[2048];
    pad[0] = static_cast<char>(depth);
    pad[sizeof(pad) - 1] = static_cast<char>(TestStackOverflow(depth + 1));
    return pad[0] + pad[sizeof(pad) - 1];
}

typedef int(__stdcall* SErrDisplayError_t)(uint32_t, const char*, int, const char*, int, uint32_t, uint32_t);

__attribute__((noinline)) static void RunCrashTest(const char* kind) {
    if (lstrcmpA(kind, "stack") == 0) {
        TestStackOverflow(0);
    } else if (lstrcmpA(kind, "fatal") == 0) {
        // the game's own fatal error box, as if the game had found a fatal condition
        reinterpret_cast<SErrDisplayError_t>(kSErrDisplayError)(0x85100086, "TemucraftCrash test", 1, "TemucraftCrash crash test (fatal error)", 0, 1, 0x11111111);
    } else {
        volatile uintptr_t bad = 0x10;
        *reinterpret_cast<volatile uint32_t*>(bad) = 0x7C;  // write to a null pointer
    }
}

// ---------------------------------------------------------------------------------------------------------------
// debug mode (TemucraftDebug.txt in the game folder): Lua errors (game and login screens) and, with
// TemucraftDebug-packets.txt too, every packet by name go to Logs\TemucraftDebug.log. Off by default.

static bool g_debug;
static bool g_debugPackets;
static wchar_t g_debugLog[MAX_PATH];
static CRITICAL_SECTION g_debugLock;
static bool g_debugLockOk;
static char g_packetBuf[32768];
static size_t g_packetLen;

static void DebugAppend(const char* data, size_t size) {
    HANDLE h = CreateFileW(g_debugLog, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, data, static_cast<DWORD>(size), &written, nullptr);
    CloseHandle(h);
}

static void DebugStamp(Buf& b) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    b.ch('[');
    b.ch(static_cast<char>('0' + t.wHour / 10)); b.ch(static_cast<char>('0' + t.wHour % 10)); b.ch(':');
    b.ch(static_cast<char>('0' + t.wMinute / 10)); b.ch(static_cast<char>('0' + t.wMinute % 10)); b.ch(':');
    b.ch(static_cast<char>('0' + t.wSecond / 10)); b.ch(static_cast<char>('0' + t.wSecond % 10)); b.str("] ");
}

static void DebugFlushPackets() {
    if (g_packetLen) {
        DebugAppend(g_packetBuf, g_packetLen);
        g_packetLen = 0;
    }
}

// one line: "[hh:mm:ss] kind: text" (text cut at 1000 characters, line breaks kept)
static void DebugLine(const char* kind, const char* text, size_t len) {
    if (!g_debug || !g_debugLockOk) return;
    char line[1100];
    Buf b(line, sizeof(line));
    DebugStamp(b);
    b.str(kind);
    b.str(": ");
    b.strn(text, len < 1000 ? len : 1000);
    b.nl();
    EnterCriticalSection(&g_debugLock);
    DebugFlushPackets();   // keep the order of lines
    DebugAppend(line, b.len);
    LeaveCriticalSection(&g_debugLock);
}

static void DebugPacket(bool fromServer, uint32_t opcode, uint32_t size) {
    if (!g_debugPackets || !g_debugLockOk) return;
    char line[128];
    Buf b(line, sizeof(line));
    DebugStamp(b);
    b.str(fromServer ? "S> " : "C> ");
    const char* name = OpcodeName(opcode);
    b.str(name ? name : "?");
    b.str(" (");
    b.hex0x(opcode, 3);
    b.str(") ");
    b.dec(size);
    b.nl();
    EnterCriticalSection(&g_debugLock);
    if (g_packetLen + b.len > sizeof(g_packetBuf)) DebugFlushPackets();
    memcpy(g_packetBuf + g_packetLen, line, b.len);
    g_packetLen += b.len;
    LeaveCriticalSection(&g_debugLock);
}

// ---------------------------------------------------------------------------------------------------------------
// Lua functions the game doesn't have, registered with the game's own FrameScript_RegisterFunction (0x00817F90)
// right after LoadScriptFunctions (0x005120E0), so they come back after every /reload:
//   TemucraftCopyText(text)  puts the text on the Windows clipboard (for the error log window)
//   TemucraftDebugLog(text)  adds a line to Logs\TemucraftDebug.log while debug mode is on
// Addons must check that they exist (players without this file, or with it turned off, don't have them).

static const uint32_t kRegisterFunction = 0x00817F90;
static const uint32_t kLoadScriptFunctions = 0x005120E0;
static const uint32_t kLoadScriptFunctionsCall = 0x0052AB16;   // the only call, in CGGameUI::InitializeFrameXML
typedef int(__cdecl* lua_CFunction_t)(void*);
typedef void(__cdecl* RegisterFunction_t)(const char*, lua_CFunction_t);
typedef const char*(__cdecl* lua_tolstring_t2)(void*, int, size_t*);
static bool g_luaFunctions;
static bool g_glueFunctions;

static void CopyToClipboard(const char* text, size_t len) {
    HMODULE user = GetModuleHandleW(L"user32.dll");
    if (!user) return;
    typedef BOOL(WINAPI * Open_t)(HWND);
    typedef BOOL(WINAPI * Empty_t)();
    typedef HANDLE(WINAPI * Set_t)(UINT, HANDLE);
    typedef BOOL(WINAPI * Close_t)();
    Open_t open = reinterpret_cast<Open_t>(GetProcAddress(user, "OpenClipboard"));
    Empty_t empty = reinterpret_cast<Empty_t>(GetProcAddress(user, "EmptyClipboard"));
    Set_t set = reinterpret_cast<Set_t>(GetProcAddress(user, "SetClipboardData"));
    Close_t close = reinterpret_cast<Close_t>(GetProcAddress(user, "CloseClipboard"));
    if (!open || !empty || !set || !close) return;
    if (len > 4 * 1024 * 1024) len = 4 * 1024 * 1024;
    int wide = MultiByteToWideChar(CP_UTF8, 0, text, static_cast<int>(len), nullptr, 0);
    if (wide <= 0) return;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (static_cast<SIZE_T>(wide) + 1) * sizeof(wchar_t));
    if (!mem) return;
    wchar_t* w = static_cast<wchar_t*>(GlobalLock(mem));
    if (!w) { GlobalFree(mem); return; }
    MultiByteToWideChar(CP_UTF8, 0, text, static_cast<int>(len), w, wide);
    w[wide] = 0;
    GlobalUnlock(mem);
    if (!open(nullptr)) { GlobalFree(mem); return; }
    empty();
    if (!set(13 /* CF_UNICODETEXT */, mem)) GlobalFree(mem);   // on success the clipboard owns the memory
    close();
}

static int __cdecl Lua_CopyText(void* L) {
    size_t len = 0;
    const char* text = reinterpret_cast<lua_tolstring_t2>(0x0084E0E0)(L, 1, &len);
    if (text && len) CopyToClipboard(text, len);
    return 0;
}

static int __cdecl Lua_DebugLog(void* L) {
    size_t len = 0;
    const char* text = reinterpret_cast<lua_tolstring_t2>(0x0084E0E0)(L, 1, &len);
    if (text && len) DebugLine("Addon", text, len);
    return 0;
}

// the performance monitor's (perf_win.inc): they do nothing while it is off
static int __cdecl Lua_PerfMark(void* L);
static int __cdecl Lua_PerfSpan(void* L);

static void RegisterOurFunctions() {
    RegisterFunction_t reg = reinterpret_cast<RegisterFunction_t>(kRegisterFunction);
    reg("TemucraftCopyText", Lua_CopyText);
    reg("TemucraftDebugLog", Lua_DebugLog);
    reg("TemucraftPerfMark", Lua_PerfMark);
    reg("TemucraftPerfSpan", Lua_PerfSpan);
}

static void __cdecl h_LoadScriptFunctions() {
    reinterpret_cast<void(__cdecl*)()>(kLoadScriptFunctions)();
    RegisterOurFunctions();
}

// The login screens have their own Lua state, set up again by CGlueMgr (FrameScript_Flush, then each
// ...RegisterScriptFunctions). CharacterCreateRegisterScriptFunctions (0x004DFD00) is called from one place,
// 0x004DA730; that call goes to h_GlueRegister, so the functions are there at the login and character screens too.
static const uint32_t kCharCreateRegister = 0x004DFD00;
static const uint32_t kCharCreateRegisterCall = 0x004DA730;

static void __cdecl h_GlueRegister() {
    reinterpret_cast<void(__cdecl*)()>(kCharCreateRegister)();
    RegisterOurFunctions();
}

static bool RedirectCall(uint32_t address, const uint8_t expect[5], void* to) {
    uint8_t a[5];
    if (!SafeRead(address, a, 5) || memcmp(a, expect, 5) != 0) return false;
    uint8_t* at = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(address));
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(to) - reinterpret_cast<uintptr_t>(at + 5));
    DWORD old = 0;
    if (!VirtualProtect(at + 1, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at + 1, &rel, 4);
    DWORD ignored;
    VirtualProtect(at + 1, 4, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), at, 5);
    return true;
}

// points the one call to LoadScriptFunctions at h_LoadScriptFunctions (only when the call and both functions look
// like the 12340 client's)
static bool InstallLuaFunctions() {
    static const uint8_t call[] = {0xE8, 0xC5, 0x75, 0xFE, 0xFF};
    static const uint8_t reg[] = {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x0C, 0x56};
    static const uint8_t load[] = {0x56, 0xE8};
    static const uint8_t tolstring[] = {0x55, 0x8B, 0xEC, 0x56};
    uint8_t a[5], b[7], c[2], d[4];
    if (!SafeRead(kLoadScriptFunctionsCall, a, 5) || memcmp(a, call, 5) != 0) return false;
    if (!SafeRead(kRegisterFunction, b, 7) || memcmp(b, reg, 7) != 0) return false;
    if (!SafeRead(kLoadScriptFunctions, c, 2) || memcmp(c, load, 2) != 0) return false;
    if (!SafeRead(0x0084E0E0, d, 4) || memcmp(d, tolstring, 4) != 0) return false;
    if (!RedirectCall(kLoadScriptFunctionsCall, call, reinterpret_cast<void*>(&h_LoadScriptFunctions))) return false;
    // the login screens (only when CharacterCreateRegisterScriptFunctions starts as in 12340: push esi; xor esi,esi)
    static const uint8_t glueCall[] = {0xE8, 0xCB, 0x55, 0x00, 0x00};
    static const uint8_t glueFn[] = {0x56, 0x33, 0xF6};
    uint8_t e[3];
    if (SafeRead(kCharCreateRegister, e, 3) && memcmp(e, glueFn, 3) == 0)
        g_glueFunctions = RedirectCall(kCharCreateRegisterCall, glueCall, reinterpret_cast<void*>(&h_GlueRegister));
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// detours (they only record, then call the game's own code)

typedef int(__thiscall* ProcessMessage_t)(void*, uint32_t, void*, int);
typedef void(__thiscall* NetSend_t)(void*, void*);
typedef int(__cdecl* LuaHandleError_t)(void*);
typedef int(__cdecl* lua_type_t)(void*, int);
typedef const char*(__cdecl* lua_tolstring_t)(void*, int, size_t*);

#include "perf_win.inc"

static ProcessMessage_t o_ProcessMessage;
static NetSend_t o_NetSend;
static LuaHandleError_t o_LuaHandleError;
static SErrDisplayError_t o_SErrDisplayError;

static int __fastcall h_ProcessMessage(void* self, void* /*edx*/, uint32_t time, void* msg, int a4) {
    const DataStore* m = static_cast<const DataStore*>(msg);
    if (m && m->data && m->read >= m->base && m->read + 2 <= m->size) {
        uint16_t opcode;
        const uint8_t* at = m->data + (m->read - m->base);
        memcpy(&opcode, at, 2);
        const uint8_t* body = at + 2;
        uint32_t bodySize = m->size - m->read - 2;
        DWORD now = GetTickCount();
        g_in.Push(now, opcode, bodySize, body, bodySize < static_cast<uint32_t>(HEAD_BYTES) ? bodySize : static_cast<uint32_t>(HEAD_BYTES));
        DebugPacket(true, opcode, bodySize);
        if (opcode == SMSG_CHAR_ENUM) {
            static CharList list;  // server packets come in on the main thread only
            if (ParseCharEnum(body, bodySize, &list)) {
                list.tick = now;
                g_charList.Store(list);
            }
        }
    }
    return o_ProcessMessage(self, time, msg, a4);
}

static void __fastcall h_NetSend(void* self, void* /*edx*/, void* msg) {
    const DataStore* m = static_cast<const DataStore*>(msg);
    if (m && m->data && m->read >= m->base && m->read + 4 <= m->size) {
        const uint8_t* at = m->data + (m->read - m->base);
        uint32_t opcode;
        memcpy(&opcode, at, 4);
        g_out.Push(GetTickCount(), opcode, m->size - m->read - 4);
        DebugPacket(false, opcode, m->size - m->read - 4);
        if (opcode == CMSG_CHAR_CREATE) {
            static CharCreate create;
            if (ParseCharCreate(at + 4, m->size - m->read - 4, &create)) {
                create.tick = GetTickCount();
                g_charCreate.Store(create);
            }
        }
        if (g_testMode && opcode == CMSG_MESSAGECHAT) {
            char kind[16];
            if (FindTestMarker(at, m->size - m->read, kind, sizeof(kind))) RunCrashTest(kind);
        }
    }
    o_NetSend(self, msg);
}

static int __cdecl h_LuaHandleError(void* L) {
    if (L && reinterpret_cast<lua_type_t>(kLuaType)(L, -1) == 4 /* LUA_TSTRING */) {
        size_t len = 0;
        const char* text = reinterpret_cast<lua_tolstring_t>(kLuaToLString)(L, -1, &len);
        if (text) {
            g_lua.Push(GetTickCount(), text, len);
            DebugLine("Lua error", text, len);
        }
    }
    return o_LuaHandleError(L);
}

static void ReportFatal(uint32_t code, const char* file, int line, const char* text, uint32_t caller);

static int __stdcall h_SErrDisplayError(uint32_t code, const char* file, int line, const char* text, int recoverable,
                                        uint32_t exitcode, uint32_t unknown) {
    if (!recoverable) ReportFatal(code, file, line, text, reinterpret_cast<uint32_t>(__builtin_return_address(0)));
    return o_SErrDisplayError(code, file, line, text, recoverable, exitcode, unknown);
}

static InlineHook g_hookIn = {kProcessMessage, 10, {0x55, 0x8B, 0xEC, 0x83, 0x05, 0x38, 0xD6, 0xC5, 0x00, 0x01},
                              reinterpret_cast<void*>(h_ProcessMessage), reinterpret_cast<void**>(&o_ProcessMessage), false};
static InlineHook g_hookOut = {kNetSend, 6, {0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1},
                               reinterpret_cast<void*>(h_NetSend), reinterpret_cast<void**>(&o_NetSend), false};
static InlineHook g_hookLua = {kLuaHandleError, 5, {0x55, 0x8B, 0xEC, 0x51, 0x56},
                               reinterpret_cast<void*>(h_LuaHandleError), reinterpret_cast<void**>(&o_LuaHandleError), false};
static InlineHook g_hookFatal = {kSErrDisplayError, 6, {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x0C},
                                 reinterpret_cast<void*>(h_SErrDisplayError), reinterpret_cast<void**>(&o_SErrDisplayError), false};

// ---------------------------------------------------------------------------------------------------------------
// the crash path

struct Pending {
    int kind;
    DWORD threadId;
    EXCEPTION_RECORD record;
    CONTEXT context;
    bool hasRecord;
    char message[600];
    uint32_t caller;  // KIND_FATAL: who called SErrDisplayError
    uint32_t tick;
};

static Pending g_pending;
static volatile LONG g_reported;     // one report per run
static volatile LONG g_writing;
static HANDLE g_go, g_done, g_writer;
static DWORD g_writerId;
static volatile bool g_exiting;

// the last fatal-looking exception the vectored handler saw (it may still be caught by the game)
static volatile LONG g_snapLock;
static EXCEPTION_RECORD g_snapRecord;
static CONTEXT g_snapContext;
static DWORD g_snapThread;
static DWORD g_snapTick;
static bool g_snapValid;

static LPTOP_LEVEL_EXCEPTION_FILTER g_chain;   // the game's own filter (Storm's ExceptionFilterWin32)
static LPTOP_LEVEL_EXCEPTION_FILTER g_other;   // a filter another DLL put in front of ours later
typedef LPTOP_LEVEL_EXCEPTION_FILTER(WINAPI* SetUEF_t)(LPTOP_LEVEL_EXCEPTION_FILTER);
static SetUEF_t g_realSetUEF;

static void WriteReport();

static void RunWriter() {
    bool writerAlive = g_writer && !g_exiting && WaitForSingleObject(g_writer, 0) == WAIT_TIMEOUT &&
                       GetCurrentThreadId() != g_writerId;
    if (writerAlive) {
        // the report is made on our own thread, which has a fresh stack (a stack overflow leaves almost none here)
        ResetEvent(g_done);
        SetEvent(g_go);
        WaitForSingleObject(g_done, 30000);
    } else {
        // the game is closing (other threads are gone): write it here
        InterlockedExchange(&g_writing, 1);
        WriteReport();
        InterlockedExchange(&g_writing, 0);
    }
}

static void ReportException(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return;
    if (InterlockedCompareExchange(&g_reported, 1, 0) != 0) return;
    g_pending.kind = KIND_EXCEPTION;
    g_pending.threadId = GetCurrentThreadId();
    memcpy(&g_pending.record, ep->ExceptionRecord, sizeof(EXCEPTION_RECORD));
    memcpy(&g_pending.context, ep->ContextRecord, sizeof(CONTEXT));
    g_pending.hasRecord = true;
    g_pending.message[0] = 0;
    g_pending.tick = GetTickCount();
    RunWriter();
}

static void ReportFatal(uint32_t code, const char* file, int line, const char* text, uint32_t caller) {
    if (InterlockedCompareExchange(&g_reported, 1, 0) != 0) return;
    Pending& p = g_pending;
    p.threadId = GetCurrentThreadId();
    p.tick = GetTickCount();
    p.caller = caller;

    // the game's text and where it came from
    char t[400], f[160];
    SafeString(text, t, sizeof(t));
    SafeString(file, f, sizeof(f));
    Buf m(p.message, sizeof(p.message));
    m.str(t);
    if (f[0]) {
        m.str(" (");
        m.str(f);
        if (line > 0 && line < 100000) {
            m.ch(':');
            m.dec(static_cast<uint32_t>(line));
        }
        m.ch(')');
    }

    // if this thread hit an exception just before, that is most likely the real cause
    bool useSnap = false;
    while (InterlockedCompareExchange(&g_snapLock, 1, 0) != 0) Sleep(0);
    if (g_snapValid && g_snapThread == p.threadId && p.tick - g_snapTick < 3000) {
        memcpy(&p.record, &g_snapRecord, sizeof(EXCEPTION_RECORD));
        memcpy(&p.context, &g_snapContext, sizeof(CONTEXT));
        useSnap = true;
    }
    InterlockedExchange(&g_snapLock, 0);

    if (useSnap) {
        p.kind = KIND_EXCEPTION;
        p.hasRecord = true;
    } else {
        p.kind = KIND_FATAL;
        p.hasRecord = false;
        memset(&p.record, 0, sizeof(p.record));
        p.record.ExceptionCode = code;
        p.record.ExceptionAddress = reinterpret_cast<PVOID>(static_cast<uintptr_t>(caller));
        RtlCaptureContext(&p.context);
    }
    RunWriter();
}

// our place in Windows' unhandled exception chain
static volatile LONG g_inFilter;
static volatile LONG g_chainCalled;

static LONG CallGameFilter(EXCEPTION_POINTERS* ep) {
    if (InterlockedExchange(&g_chainCalled, 1) != 0) return EXCEPTION_CONTINUE_SEARCH;
    return g_chain ? g_chain(ep) : EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI OurFilter(EXCEPTION_POINTERS* ep) {
    if (InterlockedExchange(&g_inFilter, 1) != 0) {
        // another filter we called passed it back to us: go on to the game's own report
        return CallGameFilter(ep);
    }
    ReportException(ep);
    LONG result;
    if (g_other) {
        result = g_other(ep);
        if (result == EXCEPTION_CONTINUE_SEARCH && !g_chainCalled) result = CallGameFilter(ep);
    } else {
        result = CallGameFilter(ep);
    }
    InterlockedExchange(&g_inFilter, 0);
    return result;
}

// Wow.exe's import of SetUnhandledExceptionFilter comes here: the game's filter goes after ours.
static LPTOP_LEVEL_EXCEPTION_FILTER WINAPI Hook_SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter) {
    if (filter == OurFilter) return g_chain;
    LPTOP_LEVEL_EXCEPTION_FILTER old = g_chain;
    g_chain = filter;
    if (g_realSetUEF) g_realSetUEF(OurFilter);
    return old;
}

static void WriterBail() {
    InterlockedExchange(&g_writing, 0);
    SetEvent(g_done);
    ExitThread(1);
}

static LONG CALLBACK Vectored(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (!IsFatalCode(code)) return EXCEPTION_CONTINUE_SEARCH;
    DWORD tid = GetCurrentThreadId();

    // the report writer itself failed: stop it, the game's own report still runs
    if (tid == g_writerId && g_writing) {
        ep->ContextRecord->Eip = reinterpret_cast<DWORD>(&WriterBail);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (g_reported) return EXCEPTION_CONTINUE_SEARCH;

    // keep a copy in case the game catches this and then stops with its own fatal error
    if (InterlockedCompareExchange(&g_snapLock, 1, 0) == 0) {
        memcpy(&g_snapRecord, ep->ExceptionRecord, sizeof(EXCEPTION_RECORD));
        memcpy(&g_snapContext, ep->ContextRecord, sizeof(CONTEXT));
        g_snapThread = tid;
        g_snapTick = GetTickCount();
        g_snapValid = true;
        InterlockedExchange(&g_snapLock, 0);
    }

    // put our filter back in front if a DLL replaced it
    if (code != EXCEPTION_STACK_OVERFLOW && g_realSetUEF) {
        LPTOP_LEVEL_EXCEPTION_FILTER prev = g_realSetUEF(OurFilter);
        if (prev && prev != OurFilter && prev != g_chain) g_other = prev;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------------------------------------------
// the report writer

typedef BOOL(WINAPI* MiniDumpWriteDump_t)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
typedef BOOL(WINAPI* SymInitialize_t)(HANDLE, PCSTR, BOOL);
typedef BOOL(WINAPI* StackWalk64_t)(DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID, PREAD_PROCESS_MEMORY_ROUTINE64,
                                    PFUNCTION_TABLE_ACCESS_ROUTINE64, PGET_MODULE_BASE_ROUTINE64, PTRANSLATE_ADDRESS_ROUTINE64);

static HMODULE g_dbghelp;
static MiniDumpWriteDump_t p_MiniDumpWriteDump;
static SymInitialize_t p_SymInitialize;
static StackWalk64_t p_StackWalk64;
static PFUNCTION_TABLE_ACCESS_ROUTINE64 p_SymFunctionTableAccess64;
static bool g_symbolsReady;
// our own handle for dbghelp: dbghelp keeps one session per process handle, and the game's own crash report calls
// SymInitialize(GetCurrentProcess()) itself. With a different handle ours never gets in its way.
static HANDLE g_symProcess;
typedef BOOL(WINAPI* SymCleanup_t)(HANDLE);
static SymCleanup_t p_SymCleanup;

static Module g_modules[MAX_MODULES];
static unsigned g_moduleCount;
static FileEntry g_files[MAX_FILES];
static unsigned g_fileCount;
static CrashInfo g_info;
static char g_text[256 * 1024];
static PacketEntry g_inCopy[64], g_outCopy[32];
static TextEntry g_luaCopy[8];

// psapi functions from kernel32 (Windows 7 and newer), looked up at start-up
typedef BOOL(WINAPI* EnumProcessModules_t)(HANDLE, HMODULE*, DWORD, LPDWORD);
typedef BOOL(WINAPI* GetModuleInformation_t)(HANDLE, HMODULE, LPMODULEINFO, DWORD);
typedef DWORD(WINAPI* GetModuleFileNameExW_t)(HANDLE, HMODULE, LPWSTR, DWORD);
static EnumProcessModules_t g_enumModules;
static GetModuleInformation_t g_moduleInfo;
static GetModuleFileNameExW_t g_moduleFile;

static void LoadModules() {
    static HMODULE list[MAX_MODULES];
    DWORD needed = 0;
    g_moduleCount = 0;
    HANDLE proc = GetCurrentProcess();
    EnumProcessModules_t enumModules = g_enumModules;
    GetModuleInformation_t moduleInfo = g_moduleInfo;
    GetModuleFileNameExW_t moduleFile = g_moduleFile;
    if (!enumModules || !moduleInfo || !moduleFile) return;
    if (!enumModules(proc, list, sizeof(list), &needed)) return;
    unsigned n = needed / sizeof(HMODULE);
    if (n > MAX_MODULES) n = MAX_MODULES;
    HMODULE exe = GetModuleHandleW(nullptr);
    for (unsigned i = 0; i < n; i++) {
        MODULEINFO mi;
        if (!moduleInfo(proc, list[i], &mi, sizeof(mi))) continue;
        Module& m = g_modules[g_moduleCount++];
        m.base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mi.lpBaseOfDll));
        m.size = mi.SizeOfImage;
        m.mainExe = list[i] == exe;
        m.version[0] = 0;
        wchar_t path[MAX_PATH];
        DWORD len = moduleFile(proc, list[i], path, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) path[0] = 0;
        ToUtf8(path, m.path, sizeof(m.path));
        const char* slash = m.path;
        for (const char* c = m.path; *c; c++)
            if (*c == '\\' || *c == '/') slash = c + 1;
        lstrcpynA(m.name, slash[0] ? slash : "?", sizeof(m.name));
    }
}

static DWORD64 CALLBACK ModuleBase(HANDLE, DWORD64 address) {
    const Module* m = FindModule(static_cast<uint32_t>(address), g_modules, g_moduleCount);
    return m ? m->base : 0;
}

static void WalkStack(CrashInfo& c, const CONTEXT& ctx, DWORD threadId) {
    c.frameCount = 0;
    c.stackMethod = "frame pointers";
    if (p_StackWalk64 && p_SymFunctionTableAccess64) {
        HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, threadId);
        CONTEXT copy;
        memcpy(&copy, &ctx, sizeof(copy));
        STACKFRAME64 sf;
        memset(&sf, 0, sizeof(sf));
        sf.AddrPC.Offset = ctx.Eip;
        sf.AddrPC.Mode = AddrModeFlat;
        sf.AddrFrame.Offset = ctx.Ebp;
        sf.AddrFrame.Mode = AddrModeFlat;
        sf.AddrStack.Offset = ctx.Esp;
        sf.AddrStack.Mode = AddrModeFlat;
        while (c.frameCount < MAX_FRAMES &&
               p_StackWalk64(IMAGE_FILE_MACHINE_I386, g_symProcess, thread ? thread : GetCurrentThread(), &sf, &copy,
                             nullptr, p_SymFunctionTableAccess64, ModuleBase, nullptr)) {
            if (sf.AddrPC.Offset == 0) break;
            c.frames[c.frameCount++] = static_cast<uint32_t>(sf.AddrPC.Offset);
        }
        if (thread) CloseHandle(thread);
        if (c.frameCount > 1) {
            c.stackMethod = "StackWalk64";
            return;
        }
        c.frameCount = 0;
    }
    // frame pointer chain
    c.frames[c.frameCount++] = ctx.Eip;
    uint32_t ebp = ctx.Ebp;
    for (int i = 0; i < MAX_FRAMES && c.frameCount < MAX_FRAMES; i++) {
        uint32_t pair[2];
        if (ebp < ctx.Esp || ebp - ctx.Esp > 0x100000 || !SafeRead(ebp, pair, sizeof(pair))) break;
        if (!pair[1]) break;
        c.frames[c.frameCount++] = pair[1];
        if (pair[0] <= ebp) break;
        ebp = pair[0];
    }
}

static void ScanStack(CrashInfo& c, uint32_t esp) {
    c.scanCount = 0;
    c.stackTop = esp;
    static uint32_t words[1024];
    uint32_t at = esp & ~3u;
    for (int page = 0; page < 4 && c.scanCount < MAX_SCAN; page++) {
        uint32_t end = (at & ~0xFFFu) + 0x1000;
        uint32_t bytes = end - at;
        if (!SafeRead(at, words, bytes)) break;
        for (uint32_t i = 0; i < bytes / 4 && c.scanCount < MAX_SCAN; i++) {
            uint32_t v = words[i];
            const Module* m = FindModule(v, g_modules, g_moduleCount);
            if (!m || v - m->base < 0x1000) continue;
            uint8_t before[7];
            if (!SafeRead(v - 7, before, 7) || !LooksLikeReturn(before)) continue;
            if (c.scanCount && c.scan[c.scanCount - 1] == v) continue;
            c.scan[c.scanCount++] = v;
        }
        at = end;
    }
}

static void AddFiles(const wchar_t* folder, const char* prefix) {
    wchar_t pattern[MAX_PATH * 2];
    lstrcpynW(pattern, g_gameDir, MAX_PATH);
    lstrcatW(pattern, folder);
    lstrcatW(pattern, L"\\*.MPQ");
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pattern, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (g_fileCount >= MAX_FILES) break;
        FileEntry& e = g_files[g_fileCount++];
        char name[MAX_PATH];
        ToUtf8(fd.cFileName, name, sizeof(name));
        lstrcpynA(e.name, prefix, sizeof(e.name));
        int used = lstrlenA(e.name);
        lstrcpynA(e.name + used, name, static_cast<int>(sizeof(e.name)) - used);
        e.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

static void LoadFiles() {
    g_fileCount = 0;
    AddFiles(L"\\Data", "Data\\");
    // locale folders (Data\enUS and so on)
    wchar_t pattern[MAX_PATH * 2];
    lstrcpynW(pattern, g_gameDir, MAX_PATH);
    lstrcatW(pattern, L"\\Data\\*");
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pattern, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.' || lstrlenW(fd.cFileName) > 8) continue;
            wchar_t sub[32] = L"\\Data\\";
            lstrcatW(sub, fd.cFileName);
            char prefix[32];
            char loc[16];
            ToUtf8(fd.cFileName, loc, sizeof(loc));
            lstrcpyA(prefix, "Data\\");
            lstrcatA(prefix, loc);
            lstrcatA(prefix, "\\");
            AddFiles(sub, prefix);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    // sort by name (few entries: insertion sort, no heap)
    for (unsigned i = 1; i < g_fileCount; i++) {
        FileEntry e = g_files[i];
        unsigned j = i;
        while (j > 0 && lstrcmpiA(g_files[j - 1].name, e.name) > 0) {
            g_files[j] = g_files[j - 1];
            j--;
        }
        g_files[j] = e;
    }
}

static GameState g_game;

static void ReadGameState(GameState& g) {
    memset(&g, 0, sizeof(g));
    if (!g_buildOk) return;
    g.valid = true;
    for (const auto& d : kDbcs) {
        if (g.dbcCount >= MAX_DBC_COUNTS) break;
        DbcCount& e = g.dbc[g.dbcCount++];
        e.name = d.name;
        uint32_t v[2];
        e.ok = SafeRead(d.base + 8, v, sizeof(v));
        e.records = e.ok ? v[0] : 0;
        e.maxId = e.ok ? static_cast<int32_t>(v[1]) : 0;
    }
    uint32_t count = 0, data = 0;
    g.createOk = SafeRead(kRaceIndex, &g.raceIndex, 4) && SafeRead(kCustomizeFrame, &g.customizeFrame, 4) &&
                 SafeRead(kCharacter, &g.character, 4) && SafeRead(kRacesCount, &count, 4) && SafeRead(kRacesData, &data, 4);
    if (g.createOk) {
        g.raceCount = count;
        unsigned want = count < static_cast<uint32_t>(MAX_CREATE_RACES) ? count : static_cast<uint32_t>(MAX_CREATE_RACES);
        if (data && want && SafeRead(data, g.races, want * 4)) g.racesRead = want;
        uint32_t prefs[44];
        if (SafeRead(kCharPreferences, prefs, sizeof(prefs)))
            for (uint32_t p : prefs) g.prefsSet += p != 0;
    }
    uint32_t headers[64];
    g.repOk = SafeRead(kRepNumHeaders, &g.numHeaders, 4) && SafeRead(kRepHeaders, headers, sizeof(headers));
    if (g.repOk)
        for (uint32_t h : headers) g.nullHeaders += h == 0;
    g.haveList = g_charList.Load(&g.list);
    g.haveCreate = g_charCreate.Load(&g.create);
}

static void Two(Buf& b, unsigned v) {
    b.ch(static_cast<char>('0' + (v / 10) % 10));
    b.ch(static_cast<char>('0' + v % 10));
}

static bool WriteAll(const wchar_t* path, const char* data, DWORD size, bool append) {
    HANDLE h = CreateFileW(path, append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           append ? OPEN_EXISTING : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data, size, &written, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
    return ok && written == size;
}

// Keeps the newest 10 of our own .dmp files (Errors\Temucraft-*.dmp); other files are never touched.
static void TrimOldDumps(const wchar_t* errors) {
    enum { KEEP = 10, MAXN = 64 };
    static wchar_t names[MAXN][64];
    unsigned n = 0;
    wchar_t pattern[MAX_PATH * 2];
    lstrcpynW(pattern, errors, MAX_PATH);
    lstrcatW(pattern, L"\\Temucraft-*.dmp");
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pattern, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (n < MAXN && lstrlenW(fd.cFileName) < 64) lstrcpyW(names[n++], fd.cFileName);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    if (n <= KEEP) return;
    // names hold the date and time, so name order is time order
    for (unsigned i = 1; i < n; i++)
        for (unsigned j = i; j > 0 && lstrcmpW(names[j - 1], names[j]) > 0; j--) {
            wchar_t t[64];
            lstrcpyW(t, names[j]);
            lstrcpyW(names[j], names[j - 1]);
            lstrcpyW(names[j - 1], t);
        }
    for (unsigned i = 0; i + KEEP < n; i++) {
        wchar_t path[MAX_PATH * 2];
        lstrcpynW(path, errors, MAX_PATH);
        lstrcatW(path, L"\\");
        lstrcatW(path, names[i]);
        DeleteFileW(path);
    }
}

static void WriteReport() {
    Pending& p = g_pending;
    CrashInfo& c = g_info;
    memset(&c, 0, sizeof(c));

    LoadModules();
    LoadFiles();

    c.kind = p.kind;
    c.code = p.record.ExceptionCode;
    c.address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p.record.ExceptionAddress));
    if (p.hasRecord && (c.code == EXCEPTION_ACCESS_VIOLATION || c.code == EXCEPTION_IN_PAGE_ERROR) && p.record.NumberParameters >= 2) {
        c.hasAccess = true;
        c.accessType = static_cast<uint32_t>(p.record.ExceptionInformation[0]);
        c.accessAddress = static_cast<uint32_t>(p.record.ExceptionInformation[1]);
    }
    lstrcpynA(c.message, p.message, sizeof(c.message));
    c.threadId = p.threadId;
    c.mainThread = p.threadId == g_mainThreadId;
    c.hasRegisters = true;
    const CONTEXT& x = p.context;
    c.regs = {x.Eax, x.Ebx, x.Ecx, x.Edx, x.Esi, x.Edi, x.Ebp, x.Esp, x.Eip, x.EFlags};
    if (p.kind == KIND_FATAL) c.address = p.caller;
    c.tick = p.tick;
    c.startTick = g_startTick;
    c.exeStamp = g_exeStamp;
    c.buildOk = g_buildOk;
    lstrcpynA(c.hooks, g_hooks, sizeof(c.hooks));
    ToUtf8(g_exePath, c.exePath, sizeof(c.exePath));
    c.modules = g_modules;
    c.moduleCount = g_moduleCount;
    c.files = g_files;
    c.fileCount = g_fileCount;

    SYSTEMTIME st;
    GetLocalTime(&st);
    {
        Buf t(c.time, sizeof(c.time));
        t.dec(st.wYear); t.ch('-'); Two(t, st.wMonth); t.ch('-'); Two(t, st.wDay); t.ch(' ');
        Two(t, st.wHour); t.ch(':'); Two(t, st.wMinute); t.ch(':'); Two(t, st.wSecond);
    }
    char base[64];
    {
        Buf t(base, sizeof(base));
        t.str("Temucraft-");
        t.dec(st.wYear); t.ch('-'); Two(t, st.wMonth); t.ch('-'); Two(t, st.wDay); t.ch('-');
        Two(t, st.wHour); Two(t, st.wMinute); Two(t, st.wSecond);
    }

    if (!g_symbolsReady) p_StackWalk64 = nullptr;
    WalkStack(c, p.context, p.threadId);
    if (g_symbolsReady && p_SymCleanup) {
        // done with dbghelp before the game's own report uses it
        p_SymCleanup(g_symProcess);
        g_symbolsReady = false;
    }
    ScanStack(c, p.context.Esp);

    wchar_t errors[MAX_PATH * 2];
    lstrcpynW(errors, g_gameDir, MAX_PATH);
    lstrcatW(errors, L"\\Errors");
    CreateDirectoryW(errors, nullptr);
    wchar_t wbase[64];
    MultiByteToWideChar(CP_UTF8, 0, base, -1, wbase, 64);
    wchar_t txt[MAX_PATH * 2], dmp[MAX_PATH * 2];
    lstrcpyW(txt, errors); lstrcatW(txt, L"\\"); lstrcatW(txt, wbase);
    lstrcpyW(dmp, txt);
    lstrcatW(txt, L".txt");
    lstrcatW(dmp, L".dmp");

    if (p_MiniDumpWriteDump) {
        lstrcpynA(c.dumpName, base, sizeof(c.dumpName) - 5);
        lstrcatA(c.dumpName, ".dmp");
    } else {
        lstrcpyA(c.dumpError, "dbghelp.dll was not found");
    }

    unsigned inN = g_in.Snapshot(g_inCopy, 64);
    unsigned outN = g_out.Snapshot(g_outCopy, 32);
    unsigned luaN = g_lua.Snapshot(g_luaCopy, 8);

    ReadGameState(g_game);
    c.game = &g_game;

    Buf b(g_text, sizeof(g_text) - 256);
    FormatReport(b, c, g_inCopy, inN, g_in.Total(), g_outCopy, outN, g_out.Total(), g_luaCopy, luaN, g_lua.Total());
    // the text goes to disk first: if the dump fails or hangs, the report is already there
    WriteAll(txt, g_text, static_cast<DWORD>(b.len), false);

    if (p_MiniDumpWriteDump) {
        HANDLE file = CreateFileW(dmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        bool ok = false;
        DWORD error = GetLastError();
        if (file != INVALID_HANDLE_VALUE) {
            EXCEPTION_POINTERS ptrs = {&p.record, &p.context};
            MINIDUMP_EXCEPTION_INFORMATION mei = {p.threadId, &ptrs, FALSE};
            MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory |
                                                            MiniDumpWithUnloadedModules);
            ok = p_MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, &mei, nullptr, nullptr) != FALSE;
            if (!ok) error = GetLastError();
            CloseHandle(file);
        }
        if (!ok) {
            char line[96];
            Buf l(line, sizeof(line));
            l.str("Minidump could not be written (Windows error ");
            l.dec(error);
            l.str(")\r\n");
            WriteAll(txt, line, static_cast<DWORD>(l.len), true);
            DeleteFileW(dmp);  // only the empty file this run just made
        }
    }
    TrimOldDumps(errors);
}

static DWORD WINAPI WriterMain(void*) {
    // dbghelp is loaded here, after the game finished loading its DLLs (never inside DllMain). The game folder's
    // copy is used when there is one, the same one the game itself would load.
    g_dbghelp = LoadLibraryW(L"dbghelp.dll");
    if (g_dbghelp) {
        p_MiniDumpWriteDump = reinterpret_cast<MiniDumpWriteDump_t>(GetProcAddress(g_dbghelp, "MiniDumpWriteDump"));
        p_SymInitialize = reinterpret_cast<SymInitialize_t>(GetProcAddress(g_dbghelp, "SymInitialize"));
        p_StackWalk64 = reinterpret_cast<StackWalk64_t>(GetProcAddress(g_dbghelp, "StackWalk64"));
        p_SymFunctionTableAccess64 =
            reinterpret_cast<PFUNCTION_TABLE_ACCESS_ROUTINE64>(GetProcAddress(g_dbghelp, "SymFunctionTableAccess64"));
        if (p_SymInitialize) {
            // Set up now, not at crash time: then a crash while Windows is loading a DLL can't make it wait.
            // Modules are not read in (fInvadeProcess FALSE): addresses are named from our own module list.
            // No symbol server: only the game folder is searched, so nothing goes over the network.
            char dir[MAX_PATH];
            ToUtf8(g_gameDir, dir, sizeof(dir));
            p_SymCleanup = reinterpret_cast<SymCleanup_t>(GetProcAddress(g_dbghelp, "SymCleanup"));
            if (DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &g_symProcess, 0, FALSE,
                                DUPLICATE_SAME_ACCESS))
                g_symbolsReady = p_SymInitialize(g_symProcess, dir, FALSE) != FALSE;
        }
    }
    for (;;) {
        if (WaitForSingleObject(g_go, INFINITE) != WAIT_OBJECT_0) return 0;
        InterlockedExchange(&g_writing, 1);
        WriteReport();
        InterlockedExchange(&g_writing, 0);
        SetEvent(g_done);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// start-up

static bool HookImport(HMODULE module, const char* dll, const char* function, void* replacement, void** original) {
    uint8_t* base = reinterpret_cast<uint8_t*>(module);
    IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS32* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (IMAGE_IMPORT_DESCRIPTOR* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; d++) {
        if (lstrcmpiA(reinterpret_cast<const char*>(base + d->Name), dll) != 0) continue;
        if (!d->OriginalFirstThunk || !d->FirstThunk) continue;
        IMAGE_THUNK_DATA32* names = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA32* slots = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            IMAGE_IMPORT_BY_NAME* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (lstrcmpA(reinterpret_cast<const char*>(byName->Name), function) != 0) continue;
            DWORD old;
            if (!VirtualProtect(&slots->u1.Function, sizeof(DWORD), PAGE_READWRITE, &old)) return false;
            *original = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<DWORD>(replacement);
            VirtualProtect(&slots->u1.Function, sizeof(DWORD), old, &old);
            return true;
        }
    }
    return false;
}

static void Start() {
    g_startTick = GetTickCount();
    g_mainThreadId = GetCurrentThreadId();

    HMODULE exe = GetModuleHandleW(nullptr);
    DWORD len = GetModuleFileNameW(exe, g_exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;
    lstrcpyW(g_gameDir, g_exePath);
    for (int i = lstrlenW(g_gameDir) - 1; i >= 0; i--)
        if (g_gameDir[i] == L'\\' || g_gameDir[i] == L'/') {
            g_gameDir[i] = 0;
            break;
        }

    uint8_t* base = reinterpret_cast<uint8_t*>(exe);
    IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS32* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386) return;
    g_exeStamp = nt->FileHeader.TimeDateStamp;
    // only inside the 3.3.5a 12340 client at its usual address (other programs in the folder load this file too)
    // (a patched Wow.exe with another time stamp still counts when at least two hook places have the expected bytes)
    if (reinterpret_cast<uintptr_t>(exe) != 0x00400000) return;
    {
        int matching = 0;
        const InlineHook* all[] = {&g_hookIn, &g_hookOut, &g_hookLua, &g_hookFatal};
        for (const InlineHook* h : all) {
            uint8_t now[12];
            if (SafeRead(h->address, now, h->length) && memcmp(now, h->expect, h->length) == 0) matching++;
        }
        g_buildOk = g_exeStamp == kWowTimeStamp || matching >= 2;
    }
    if (!g_buildOk) return;
    if (FileExists(L"TemucraftCrash-off.txt")) return;

    char env[8];
    DWORD envLen = GetEnvironmentVariableA("TEMUCRAFT_CRASH_TEST", env, sizeof(env));
    g_testMode = FileExists(L"TemucraftCrash-test.txt") || (envLen == 1 && env[0] == '1');

    // debug mode: Logs\TemucraftDebug.log (the launcher's Debug mode option puts TemucraftDebug.txt here)
    if (FileExists(L"TemucraftDebug.txt")) {
        wchar_t logs[MAX_PATH];
        lstrcpynW(logs, g_gameDir, MAX_PATH);
        if (lstrlenW(logs) + 40 < MAX_PATH) {
            lstrcatW(logs, L"\\Logs");
            CreateDirectoryW(logs, nullptr);
            lstrcpyW(g_debugLog, logs);
            lstrcatW(g_debugLog, L"\\TemucraftDebug.log");
            InitializeCriticalSection(&g_debugLock);
            g_debugLockOk = true;
            g_debug = true;
            g_debugPackets = FileExists(L"TemucraftDebug-packets.txt");
        }
    }

    g_go = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_go || !g_done) goto fail;
    // the thread starts once Windows has finished loading the game's DLLs
    g_writer = CreateThread(nullptr, 256 * 1024, WriterMain, nullptr, 0, &g_writerId);
    if (!g_writer) goto fail;
    if (!AddVectoredExceptionHandler(1, Vectored)) goto fail;

    {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        g_realSetUEF = k32 ? reinterpret_cast<SetUEF_t>(GetProcAddress(k32, "SetUnhandledExceptionFilter")) : nullptr;
        if (!g_realSetUEF) g_realSetUEF = SetUnhandledExceptionFilter;
        if (k32) {
            g_enumModules = reinterpret_cast<EnumProcessModules_t>(GetProcAddress(k32, "K32EnumProcessModules"));
            g_moduleInfo = reinterpret_cast<GetModuleInformation_t>(GetProcAddress(k32, "K32GetModuleInformation"));
            g_moduleFile = reinterpret_cast<GetModuleFileNameExW_t>(GetProcAddress(k32, "K32GetModuleFileNameExW"));
        }
        g_chain = g_realSetUEF(OurFilter);
        void* ignored = nullptr;
        bool guard = HookImport(exe, "KERNEL32.dll", "SetUnhandledExceptionFilter",
                                reinterpret_cast<void*>(Hook_SetUnhandledExceptionFilter), &ignored);

        g_trampolines = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        Install(g_hookIn);
        Install(g_hookOut);
        {
            // the Lua hook calls lua_type and lua_tolstring: only when both start as expected
            static const uint8_t luaType[] = {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x0C};
            static const uint8_t luaToLString[] = {0x55, 0x8B, 0xEC, 0x56};
            uint8_t a[6], b[4];
            if (SafeRead(kLuaType, a, 6) && memcmp(a, luaType, 6) == 0 && SafeRead(kLuaToLString, b, 4) &&
                memcmp(b, luaToLString, 4) == 0)
                Install(g_hookLua);
        }
        Install(g_hookFatal);
        g_luaFunctions = InstallLuaFunctions();
        StartPerf();
        if (g_trampolines) {
            DWORD old;
            VirtualProtect(g_trampolines, 4096, PAGE_EXECUTE_READ, &old);
        }

        Buf h(g_hooks, sizeof(g_hooks));
        h.str("server packets "); h.str(g_hookIn.on ? "on" : "OFF");
        h.str(", client packets "); h.str(g_hookOut.on ? "on" : "OFF");
        h.str(", Lua errors "); h.str(g_hookLua.on ? "on" : "OFF");
        h.str(", fatal errors "); h.str(g_hookFatal.on ? "on" : "OFF");
        h.str(", filter guard "); h.str(guard ? "on" : "OFF");
        h.str(", Lua functions "); h.str(g_luaFunctions ? (g_glueFunctions ? "on" : "on (game only)") : "OFF");
        if (g_perf) h.str(", PERFORMANCE LOG");
        if (g_debug) h.str(g_debugPackets ? ", DEBUG MODE with packets" : ", DEBUG MODE");
        if (g_testMode) h.str(", TEST MODE");
    }
    g_active = true;
    if (g_debug) {
        char head[260];
        Buf b(head, sizeof(head));
        b.nl();
        DebugStamp(b);
        b.str("=== game started, TemucraftCrash hooks: ");
        b.str(g_hooks);
        b.nl();
        EnterCriticalSection(&g_debugLock);
        DebugAppend(head, b.len);
        LeaveCriticalSection(&g_debugLock);
    }
    return;

fail:
    // turn ourselves off: the game runs as if this file only passed version.dll calls on
    if (g_writer) {
        TerminateThread(g_writer, 0);
        CloseHandle(g_writer);
        g_writer = nullptr;
    }
    if (g_go) CloseHandle(g_go);
    if (g_done) CloseHandle(g_done);
    g_go = g_done = nullptr;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = instance;
        DisableThreadLibraryCalls(instance);
        Start();
    } else if (reason == DLL_PROCESS_DETACH) {
        // reserved != null: the game is closing. Crashes from here on are written on the crashing thread.
        g_exiting = true;
        if (g_debugPackets && reserved) DebugFlushPackets();   // the game is closing: other threads are gone
        if (g_perf && reserved) PerfFlush();
        (void)reserved;
    }
    return TRUE;
}
