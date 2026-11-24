# TemucraftCrash

A crash report helper for the Temucraft client (World of Warcraft 3.3.5a, build 12340).

When the game crashes it writes `Errors\Temucraft-<date>-<time>.txt` and a minidump
`Errors\Temucraft-<date>-<time>.dmp` next to it. The game's own crash report (`Errors\<date> Crash.txt`) still
runs as before, after ours.

The report has:

- the exception code and address, the module and offset, and the Wow.exe function name
  (from the decompiled client's function list, 20,838 functions, built into the DLL)
- the CPU registers (each one that points into code is named too)
- the crashing thread's stack (StackWalk64, or the frame pointer chain when that fails), plus the return addresses
  found on the stack, each with module, offset and function name
- the last 64 server packets received and the last 32 client packets sent: opcode name, number, size and how long
  before the crash
- the last 8 Lua errors
- for server packets, the first 16 bytes after the opcode; for `SMSG_INITIALIZE_FACTIONS` the count and for
  `SMSG_SET_FACTION_STANDING` the first reputation list index are spelled out, with a note when they are past the
  stock client's 128 slots (`CGReputationInfo::m_factionStandings[128]` at 0x00C22B70; a higher index writes over
  `m_headers[64]` at 0x00C23370 and crashes later in `CGReputationInfo::AddHeader` 0x005CFE70)
- races and characters (see "Race data in the report" below)
- the client build and which hooks were on
- the patch list (`Data\*.MPQ` and `Data\<locale>\*.MPQ`, names and sizes)
- the loaded DLLs (address range and full path)
- the name of the minidump

The game's own fatal errors that are not exceptions (the error box, "ERROR #134 Fatal condition") are reported too,
with the game's message.

## Race data in the report

Added for the crashes with the extra races. Nothing here hooks anything new: at crash time the writer only reads
fixed Wow.exe 12340 addresses with `ReadProcessMemory` (a bad pointer gives "could not read", never a second crash),
and the two packet hooks above parse two packets into fixed buffers.

- DBC sizes: records and highest id of ChrRaces, ChrClasses, CharBaseInfo, CharSections, Faction,
  FactionTemplate, EmotesTextSound and TalentTab (`WowClientDB<T>`: `m_numRecords` +0x08, `m_maxID` +0x0C, bases
  from the `// STATIC:` tags in `WoW/Source/DB/DBClient/DBClient.cpp`)
- character creation (`CCharacterCreation`): the race ids in `m_races` (0x00B6B1CC), `m_raceIndex` (0x00B6B180),
  `m_character` (0x00B6B1A0), `m_charCustomizeFrame` (0x00B6B184) and how many of the 44 `m_charPreferences[22][2]`
  (0x00B6B0D0) are set. A note is added when a race id of 22 or more is in the list (that array then writes over
  the values after it) or when there are more than 10 races (`Script_GetAvailableRaces` 0x004E1880 pushes 3 Lua
  values per race with no `lua_checkstack`)
- reputation headers: `m_numHeaders` (0x00C23470) and null pointers in `m_headers[64]`
- the last character list (`SMSG_CHAR_ENUM`): name, race, class, gender, skin, face, hair, facial hair and level
  of each character (up to 12)
- the last create request (`CMSG_CHAR_CREATE`): the same, plus the outfit

## How it loads

The file is called `version.dll` and goes next to `Wow.exe`. Wow.exe imports `VERSION.dll`
(`GetFileVersionInfoA`, `GetFileVersionInfoSizeA`, `VerQueryValueA`), and Windows looks in the game folder first, so
it loads this file. Every one of the 17 version.dll functions is passed on to Windows' own `version.dll`
(`src/proxy.s`), which is loaded the first time one of them is called.

Why this way:

- The Lexara `dinput8.dll` does not load a list of extra DLLs. It is a proxy that can be named `dinput8.dll`,
  `dsound.dll` or `version.dll`; the only other DLL it loads is one fixed name (`LuxShoulderCam.dll`), and only when
  Lexara is on.
- The Awesome WotLK patch in Wow.exe calls `LoadLibraryA("AwesomeWotlkLib.dll")` from a small piece of code at
  0x4E5CB0 (reached from 0x40B7D0). There is no room there for a second name, and changing Wow.exe again would
  touch a file other work depends on.
- Starting Wow.exe from the launcher and injecting with `CreateRemoteThread` only works when the game is started
  from the launcher, and virus scanners dislike it.
- `version.dll` is not one of the Windows "known DLLs", so the copy in the game folder is used, it needs no change to
  Wow.exe or the launcher, and it is turned on and off just by being there or not (the Mods option
  "Crash reports").

Other programs in the game folder (Repair.exe, WowError.exe) may load it too. It checks that the program is the
3.3.5a 12340 Wow.exe (loaded at 0x400000, with PE time stamp `4C2452FE` or at least two of the hook places below
with the expected bytes); anywhere else it only passes the calls on.

## What it changes in the game

All addresses are Wow.exe 3.3.5a (12340). They were taken from the wowdecomp source (`// OFFSET:` tags), checked in
the launcher's function list, and their first bytes were read from the Temucraft Wow.exe. Before each hook goes in,
the DLL compares those bytes; when they differ that hook stays off (the report says which hooks were on).

| What | Address | Source | Bytes moved |
|---|---|---|---|
| `NetClient::ProcessMessage(this, time, CDataStore*, a4)`: every server packet | 0x631FE0 | `WoW/Source/Net/NetClient/NetClient.cpp` | `55 8B EC 83 05 38 D6 C5 00 01` |
| `NetClient::Send(this, CDataStore*)`: every client packet (`ClientServices::SendPacket` 0x406F40 and `ClientServices::Send2` 0x6B0B50 both call it) | 0x632B50 | same | `55 8B EC 56 8B F1` |
| `FrameScript_HandleError(lua_State*)`: the Lua error function, message on top of the Lua stack | 0x819730 | `Engine/Source/FrameScript/FrameScript.cpp` | `55 8B EC 51 56` |
| `SErrDisplayError(code, file, line, text, recoverable, exitcode, x)`: the game's error box | 0x771D10 | `Engine/Source/Storm/Source/Win32/SErr.cpp` | `55 8B EC 8B 45 0C` |
| `lua_type` and `lua_tolstring` (called, not hooked) | 0x84DEB0, 0x84E0E0 | `Engine/Source/lua-5.1/src/lapi.c` | |
| Wow.exe's import of `kernel32!SetUnhandledExceptionFilter` (data, not code) | import table | | |

The opcode is read the same way the game reads it: `CDataStore` has `m_data` at +4, `m_base` +8, `m_size` +0x10,
`m_read` +0x14; a server packet starts with a 16-bit opcode, a client packet with a 32-bit one.

Each hook is a 5 byte jump to a small function that writes one entry into a ring buffer and then jumps on to the
game's own code. The ring buffers are lock free (one atomic add per packet, no waiting), so normal play is not slowed
down. None of these addresses are in AzerothCore's default Warden memory checks (`warden_checks`).

## How a crash is caught

- At start-up our unhandled exception filter goes in first. The game's own filter (Storm `ExceptionFilterWin32`,
  set by `SErrCatchUnhandledExceptions` at 0x772B20) goes in through Wow.exe's import table, so ours stays in front
  and calls the game's filter after writing the report. The game's report is unchanged.
- A vectored exception handler sees fatal exceptions first (access violation, illegal instruction, divide by zero,
  stack overflow, heap corruption and so on). It only keeps a copy (the game catches some of these itself) and puts
  our filter back in front if another DLL replaced it. It never writes a report for an exception the game may still
  handle.
- When the game stops with its own fatal error (`SErrDisplayError` with recoverable = 0), the report is written
  there, using the copy from the vectored handler when the same thread had an exception just before.
- The report is made on a thread of our own that waits from start-up (so it has a fresh stack even after a stack
  overflow). The crashing thread waits for it at most 30 seconds. If the writer itself crashes, it stops and the
  game's own report goes on. When the game is closing (other threads are gone), the report is written on the
  crashing thread.
- The report text is built in fixed buffers (no heap) and written to disk before the minidump.
- The minidump uses `MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory | MiniDumpWithUnloadedModules`
  (usually well under a few MB). Only the newest 10 `Temucraft-*.dmp` are kept; nothing else is ever deleted.
- `dbghelp.dll` is loaded by the writer thread after start-up (the same copy the game would load). It is set up with
  a process handle of our own, not `GetCurrentProcess()`, and closed (`SymCleanup`) right after our stack walk,
  because the game's own report calls `SymInitialize(GetCurrentProcess())` (Storm `sSymInitialize`, 0x77D630) and
  must not find it in use. No modules are read in and the only search path is the game folder: no symbol server,
  no network access at all.

If anything at start-up fails (an event, the writer thread, the vectored handler), the DLL turns itself off and
only passes version.dll calls on.

## Switches (files in the game folder)

- `TemucraftCrash-off.txt`: the DLL does nothing but pass version.dll calls on.
- `TemucraftCrash-test.txt` (or the environment variable `TEMUCRAFT_CRASH_TEST=1`): test mode, see below.

## Testing in the test client

1. Copy `build/version.dll` (or `5) Release/options/Crash reports/On/version.dll`) into the test client folder,
   next to Wow.exe (for example `C:\Users\...\Desktop\Temu Test\Test`).
2. Make an empty file `TemucraftCrash-test.txt` in the same folder.
3. Start the game and log in with a character.
4. Optional: make a Lua error first so it shows in the report: `/run error("crash report test")`.
5. Type one of these in chat (the message is never sent; the game crashes before it goes out):
   - `/s tc-crash-test`: writes to a null pointer (access violation)
   - `/s tc-crash-test stack`: a stack overflow
   - `/s tc-crash-test fatal`: the game's own fatal error box (ERROR #134)
6. Look in `Errors`: `Temucraft-<date>-<time>.txt` and `.dmp`, and the game's own `<date> Crash.txt` as before.
   The report should say `Hooks: server packets on, client packets on, Lua errors on, fatal errors on,
   filter guard on, TEST MODE`, list the packets from just before, and the crash should be in `version.dll`
   with the game's chat code (the callers of `NetClient::Send`) below it on the stack.
7. Delete `TemucraftCrash-test.txt` when done. Without it the chat text does nothing.

## Building

On Linux (the build was made in a container with Ubuntu's `g++-mingw-w64-i686`):

    ./build.sh

It runs the tests (`tests/test_core.cpp`: text formatting, name lookups, ring buffers with 4 threads writing while
one reads, the test marker, the return address check, the character list and create parsers, a whole sample report), builds `build/version.dll` and checks
it (`tools/check_pe.sh`: 32-bit x86 DLL, the 17 version.dll exports and nothing else, imports only kernel32 and
msvcrt). `./build/test_core --sample` prints a sample report.

`data/functions.inc` and `data/opcodes.inc` are made by `tools/gen_data.py` from the launcher's
`src/Launcher.Core/wow-functions.txt.gz` and the server's `src/server/game/Server/Protocol/Opcodes.h`:

    FUNCTIONS_GZ=".../4) Launcher/src/Launcher.Core/wow-functions.txt.gz" \
    OPCODES_H=".../1) Source/src/server/game/Server/Protocol/Opcodes.h" ./build.sh

## Files

- `src/core.h`, `src/core.cpp`: text, names, ring buffers, report layout (no Windows, no heap; tested on Linux)
- `src/win_main.cpp`: start-up, hooks, exception handling, the report writer, the minidump
- `src/proxy.s`, `src/version.def`: the version.dll exports
- `tools/gen_data.py`, `tools/check_pe.sh`, `tests/test_core.cpp`, `build.sh`
