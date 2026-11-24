# version.dll exports. Each one jumps to the same function in Windows' own version.dll,
# which is loaded the first time any of them is called (not while the game is loading DLLs).
# STUB name, index, bytes of arguments (they are stdcall, so a failed call must pop them).
    .text
    .macro STUB name, idx, bytes
    .globl _tcp_\name
_tcp_\name:
    movl _g_versionReal+4*\idx, %eax
    testl %eax, %eax
    jz 1f
    jmp *%eax
1:  pushl $\idx
    call _ResolveVersion
    addl $4, %esp
    testl %eax, %eax
    jz 2f
    jmp *%eax
2:  ret $\bytes
    .endm

    STUB GetFileVersionInfoA, 0, 16
    STUB GetFileVersionInfoByHandle, 1, 16
    STUB GetFileVersionInfoExA, 2, 20
    STUB GetFileVersionInfoExW, 3, 20
    STUB GetFileVersionInfoSizeA, 4, 8
    STUB GetFileVersionInfoSizeExA, 5, 12
    STUB GetFileVersionInfoSizeExW, 6, 12
    STUB GetFileVersionInfoSizeW, 7, 8
    STUB GetFileVersionInfoW, 8, 16
    STUB VerFindFileA, 9, 32
    STUB VerFindFileW, 10, 32
    STUB VerInstallFileA, 11, 32
    STUB VerInstallFileW, 12, 32
    STUB VerLanguageNameA, 13, 12
    STUB VerLanguageNameW, 14, 12
    STUB VerQueryValueA, 15, 16
    STUB VerQueryValueW, 16, 16
