#!/usr/bin/env python3
"""Makes the two data files the DLL embeds.

  data/functions.inc  Wow.exe function starts and names, from the launcher's wow-functions.txt.gz
                      (lines like "00631FE0 NetClient::ProcessMessage", sorted by address).
  data/opcodes.inc    Opcode names by number, from the server's Opcodes.h (3.3.5 numbering).

Usage: gen_data.py <wow-functions.txt.gz> <Opcodes.h> <out folder>
"""
import gzip
import re
import sys


def c_string(text):
    out = []
    for ch in text.encode("utf-8"):
        if ch in (0x22, 0x5C):  # " and backslash
            out.append("\\" + chr(ch))
        elif 0x20 <= ch < 0x7F and ch != 0x3F:  # leave out ? so no trigraphs
            out.append(chr(ch))
        else:
            out.append("\\%03o" % ch)
    return "".join(out)


def functions(path, out):
    rows = []
    with gzip.open(path, "rt", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\r\n")
            space = line.find(" ")
            if space <= 0:
                continue
            try:
                start = int(line[:space], 16)
            except ValueError:
                continue
            name = line[space + 1:].strip()
            if name:
                rows.append((start, name[:120]))
    rows.sort()
    # one name per start: keep the first
    unique = []
    for start, name in rows:
        if unique and unique[-1][0] == start:
            continue
        unique.append((start, name))
    with open(out, "w", encoding="ascii") as f:
        f.write("// Made by tools/gen_data.py from wow-functions.txt.gz. Do not edit.\n")
        f.write("static const unsigned kFuncCount = %d;\n" % len(unique))
        f.write("static const unsigned kFuncStarts[] = {\n")
        for i in range(0, len(unique), 8):
            f.write(",".join("0x%08X" % s for s, _ in unique[i:i + 8]) + ",\n")
        f.write("};\n")
        offsets = []
        pos = 0
        for _, name in unique:
            offsets.append(pos)
            pos += len(name.encode("utf-8")) + 1
        f.write("static const unsigned kFuncNameAt[] = {\n")
        for i in range(0, len(offsets), 12):
            f.write(",".join(str(o) for o in offsets[i:i + 12]) + ",\n")
        f.write("};\n")
        f.write("static const char kFuncNames[] =\n")
        for _, name in unique:
            f.write('"%s\\0"\n' % c_string(name))
        f.write(";\n")
    return len(unique)


def opcodes(path, out):
    names = {}
    count = 0
    pattern = re.compile(r"^\s*((?:SMSG|CMSG|MSG|UMSG)_\w+)\s*=\s*0x([0-9A-Fa-f]+)\s*,")
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = pattern.match(line)
            if m:
                number = int(m.group(2), 16)
                names.setdefault(number, m.group(1))
                continue
            m = re.match(r"^\s*NUM_MSG_TYPES\s*=\s*0x([0-9A-Fa-f]+)", line)
            if m:
                count = int(m.group(1), 16)
    if count == 0:
        count = max(names) + 1
    with open(out, "w", encoding="ascii") as f:
        f.write("// Made by tools/gen_data.py from Opcodes.h. Do not edit.\n")
        f.write("static const unsigned kOpcodeCount = 0x%X;\n" % count)
        f.write("static const char* const kOpcodeNames[] = {\n")
        for n in range(count):
            f.write('"%s",\n' % names.get(n, ""))
        f.write("};\n")
    return len(names), count


if __name__ == "__main__":
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(1)
    n = functions(sys.argv[1], sys.argv[3] + "/functions.inc")
    o, c = opcodes(sys.argv[2], sys.argv[3] + "/opcodes.inc")
    print("functions: %d, opcodes: %d named of 0x%X" % (n, o, c))
