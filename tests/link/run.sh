#!/bin/sh
# tests/link/run.sh — phase-0 link test on a non-Windows host.
#
# Generates the phase-0 objects, checks they are reproducible, links the
# no-CRT program with lld-link against a kernel32 import library generated
# from a .def file (no Microsoft files needed), and cross-checks the image,
# its unwind data and the PDB with LLVM's independent readers.
#
# Final acceptance (link.exe + running the program) needs Windows on Arm:
# see tests/windows/p0-accept.cmd.
set -eu

BUILD=${1:-build}
OUT="$BUILD/p0"
mkdir -p "$OUT"
fail=0
check() { # check <description> <command...>
    desc=$1; shift
    if "$@" >/dev/null 2>&1; then echo "  ok    $desc"; else echo "  FAIL  $desc"; fail=1; fi
}

for t in llvm-dlltool lld-link llvm-readobj llvm-objdump llvm-pdbutil; do
    command -v $t >/dev/null || { echo "link test SKIPPED: $t not found"; exit 0; }
done

echo "phase-0 link test"
"$BUILD/p0demo" nocrt "$OUT/p0_nocrt.obj" >/dev/null
"$BUILD/p0demo" crt "$OUT/p0_crt.obj" >/dev/null
cp "$OUT/p0_nocrt.obj" "$OUT/p0_nocrt.first.obj"
"$BUILD/p0demo" nocrt "$OUT/p0_nocrt.obj" >/dev/null
check "object output is reproducible" cmp "$OUT/p0_nocrt.obj" "$OUT/p0_nocrt.first.obj"

printf 'LIBRARY kernel32.dll\nEXPORTS\nGetStdHandle\nWriteFile\nExitProcess\n' >"$OUT/kernel32.def"
llvm-dlltool -m arm64 -d "$OUT/kernel32.def" -l "$OUT/kernel32.lib"

check "lld-link links the no-CRT program" \
    lld-link /machine:arm64 /entry:p0_start /subsystem:console /debug /libpath:"$OUT" \
    /out:"$OUT/p0_nocrt.exe" "$OUT/p0_nocrt.obj"

check "ncc-uwcheck: every unwind record in the linked image checks out" \
    "$BUILD/ncc-uwcheck" --strict "$OUT/p0_nocrt.exe"

# Unwind data from an independent producer: clang-compiled code (including
# frame forms ncc never emits, such as unchained leaves) linked into the same
# image. The checker may skip forms it does not model but must never fail one.
if clang --target=aarch64-pc-windows-msvc -O2 -fno-stack-protector -c tests/link/foreign.c \
        -o "$OUT/foreign.obj" 2>/dev/null; then
    check "lld-link links ncc and clang objects together" \
        lld-link /machine:arm64 /entry:p0_start /subsystem:console /opt:noref \
        /libpath:"$OUT" /out:"$OUT/mixed.exe" "$OUT/p0_nocrt.obj" "$OUT/foreign.obj"
    check "ncc-uwcheck: clang-generated unwind data checks out (skips allowed, no failures)" \
        "$BUILD/ncc-uwcheck" "$OUT/mixed.exe"
else
    echo "  skip  clang cannot target aarch64-pc-windows-msvc; mixed-image check skipped"
fi

readobj_clean() { ! llvm-readobj "$@" 2>&1 | grep -qiE "warning|error"; }
check "image headers parse" readobj_clean --file-headers --sections "$OUT/p0_nocrt.exe"
check "unwind data parses without warnings" readobj_clean --unwind "$OUT/p0_nocrt.exe"

nrec=$(llvm-readobj --unwind "$OUT/p0_nocrt.exe" | grep -c "Function:" || true)
check "six functions have unwind records (two are frameless leaves)" test "$nrec" -eq 6

imports=$(llvm-readobj --coff-imports "$OUT/p0_nocrt.exe" | grep -c "Symbol:" || true)
check "three kernel32 imports" test "$imports" -eq 3

nproc=$(llvm-pdbutil dump -symbols "$OUT/p0_nocrt.pdb" | grep -cE "S_[GL]PROC32" || true)
check "PDB has eight procedures" test "$nproc" -eq 8
nlines=$(llvm-pdbutil dump -l "$OUT/p0_nocrt.pdb" | grep -c "line/addr entries" || true)
check "PDB has line tables" test "$nlines" -ge 8

relocs=$(llvm-readobj --relocations "$OUT/p0_crt.obj")
check "CRT object: TLS uses SECREL_HIGH12A" sh -c "echo \"\$0\" | grep -q 'SECREL_HIGH12A tls_counter'" "$relocs"
check "CRT object: TLS uses SECREL_LOW12A" sh -c "echo \"\$0\" | grep -q 'SECREL_LOW12A tls_counter'" "$relocs"
check "CRT object: calls the CRT __chkstk" sh -c "echo \"\$0\" | grep -q 'BRANCH26 __chkstk'" "$relocs"
check "CRT object parses without warnings" readobj_clean --all "$OUT/p0_crt.obj"

if [ $fail -ne 0 ]; then echo "phase-0 link test FAILED"; exit 1; fi
echo "phase-0 link test passed"
