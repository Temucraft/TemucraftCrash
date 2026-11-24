#!/bin/sh
# Checks a built version.dll: 32-bit x86 DLL, exports all 17 version.dll functions (and nothing else),
# imports only DLLs that every Windows has (no MinGW runtime DLLs, no network DLLs).
set -eu
dll=$1
dump=$(objdump -p "$dll")
fail=0

echo "$dump" | grep -q 'file format pei-i386' || { echo "FAIL: not a 32-bit x86 PE file"; fail=1; }
echo "$dump" | grep -q 'Characteristics.*' || true
echo "$dump" | grep -qi 'DLL' || { echo "FAIL: not a DLL"; fail=1; }

exports=$(echo "$dump" | sed -n '/\[Ordinal\/Name Pointer\] Table/,/^$/p' | grep -E '^\s+\[ *[0-9]+\] [A-Za-z]' | awk '{print $NF}' | sort)
want="GetFileVersionInfoA GetFileVersionInfoByHandle GetFileVersionInfoExA GetFileVersionInfoExW GetFileVersionInfoSizeA
GetFileVersionInfoSizeExA GetFileVersionInfoSizeExW GetFileVersionInfoSizeW GetFileVersionInfoW VerFindFileA VerFindFileW
VerInstallFileA VerInstallFileW VerLanguageNameA VerLanguageNameW VerQueryValueA VerQueryValueW"
for name in $want; do
    echo "$exports" | grep -qx "$name" || { echo "FAIL: export $name missing"; fail=1; }
done
count=$(echo "$exports" | grep -c . || true)
[ "$count" = 17 ] || { echo "FAIL: $count exports, want 17"; fail=1; }

imports=$(echo "$dump" | grep 'DLL Name:' | awk '{print $3}' | tr 'A-Z' 'a-z' | sort -u)
for i in $imports; do
    case $i in
        kernel32.dll|msvcrt.dll) ;;
        *) echo "FAIL: imports $i"; fail=1 ;;
    esac
done
echo "exports: $count, imports: $(echo $imports)"
if echo "$dump" | grep -qiE 'ws2_32|wininet|winhttp|urlmon'; then echo "FAIL: network DLL"; fail=1; fi
[ $fail = 0 ] && echo "PE checks passed"
exit $fail
