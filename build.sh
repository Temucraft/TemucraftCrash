#!/bin/sh
# Builds TemucraftCrash (build/version.dll) on Linux and runs the self-checks.
#
# Needs: g++ (for the tests), i686-w64-mingw32-g++ (apt install g++-mingw-w64-i686), objdump, python3.
#
# To make the data files again (after the launcher's function list or the server's opcodes change):
#   FUNCTIONS_GZ=/path/to/wow-functions.txt.gz OPCODES_H=/path/to/Opcodes.h ./build.sh
set -eu
cd "$(dirname "$0")"
mkdir -p build

if [ -n "${FUNCTIONS_GZ:-}" ] && [ -n "${OPCODES_H:-}" ]; then
    python3 tools/gen_data.py "$FUNCTIONS_GZ" "$OPCODES_H" data
fi

echo "== tests (native)"
g++ -O2 -std=c++17 -Wall -Wextra -Werror -pthread tests/test_core.cpp src/core.cpp -o build/test_core
./build/test_core
g++ -O2 -std=c++17 -Wall -Wextra -Werror tests/test_perf.cpp src/perf.cpp src/core.cpp -o build/test_perf
./build/test_perf

echo "== version.dll (32-bit Windows)"
CXX=${CXX:-i686-w64-mingw32-g++}
$CXX -O2 -std=c++17 -Wall -Wextra -Werror -Wno-cast-function-type -Wno-infinite-recursion \
    -fno-exceptions -fno-rtti -fno-asynchronous-unwind-tables \
    -shared -static -static-libgcc -static-libstdc++ -s \
    -Wl,--no-insert-timestamp -Wl,--enable-stdcall-fixup -Wl,--dynamicbase -Wl,--nxcompat \
    -o build/version.dll src/win_main.cpp src/core.cpp src/perf.cpp src/proxy.s src/version.def

echo "== checks"
sh tools/check_pe.sh build/version.dll
ls -l build/version.dll
