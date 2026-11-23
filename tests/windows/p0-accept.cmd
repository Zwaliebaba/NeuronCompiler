@echo off
rem tests\windows\p0-accept.cmd — phase-0 acceptance on Windows on Arm.
rem
rem Run from the repository root in an "ARM64 Native Tools Command Prompt for
rem VS 2026" (link.exe, dumpbin and the ARM64 CRT/SDK libraries on LIB).
rem
rem   tests\windows\p0-accept.cmd [dir-with-objs]
rem
rem Takes p0_nocrt.obj and p0_crt.obj either from the given directory (e.g.
rem copied from a Linux build: they are byte-identical on every host) or, if
rem none is given, builds p0demo with clang (build.cmd) and generates them.
rem Then links both with link.exe, runs them and compares their output.
setlocal enabledelayedexpansion
set OUT=build\p0win
if not exist %OUT% mkdir %OUT%

if "%~1"=="" (
    call build.cmd || goto :fail
    build\p0demo.exe nocrt %OUT%\p0_nocrt.obj || goto :fail
    build\p0demo.exe crt %OUT%\p0_crt.obj || goto :fail
) else (
    copy /y "%~1\p0_nocrt.obj" %OUT%\ >nul || goto :fail
    copy /y "%~1\p0_crt.obj" %OUT%\ >nul || goto :fail
)

echo === link.exe, no CRT
link /nologo /machine:arm64 /entry:p0_start /subsystem:console /debug /nodefaultlib ^
     /out:%OUT%\p0_nocrt.exe %OUT%\p0_nocrt.obj kernel32.lib || goto :fail
echo === link.exe, MSVC CRT (msvcrt, ucrt, legacy_stdio_definitions via .drectve)
link /nologo /machine:arm64 /subsystem:console /debug ^
     /out:%OUT%\p0_crt.exe %OUT%\p0_crt.obj || goto :fail

echo === run
%OUT%\p0_nocrt.exe > %OUT%\nocrt.out || goto :fail
%OUT%\p0_crt.exe > %OUT%\crt.out || goto :fail

> %OUT%\nocrt.expected (
    echo NeuronCompiler phase 0: hand-built ARM64, no CRT
    echo sum of squares 1..10 = 385
    echo 8 KiB frame checksum = 1044480
    echo variadic sum^(10, 20, 30^) = 60
    echo data relocation ok
)
> %OUT%\crt.expected (
    echo hello from ncc phase 0: 42, thread-local counter = 101
    echo 8 KiB frame checksum = 1044480, variadic sum = 60
)
rem The no-CRT program writes LF line endings; normalise before comparing.
powershell -NoProfile -Command ^
  "$ok=$true; foreach($n in 'nocrt','crt'){ $a=(Get-Content %OUT%\$n.out) -join \"`n\"; $b=(Get-Content %OUT%\$n.expected) -join \"`n\"; if($a -ne $b){ Write-Host \"output mismatch: $n\"; $ok=$false } }; if(-not $ok){ exit 1 }" || goto :fail

echo === RtlVirtualUnwind cross-check of every function's unwind data
if exist build\ncc-uwcheck.exe (
    rem ncc-built image: every function must be fully checkable.
    build\ncc-uwcheck.exe --strict %OUT%\p0_nocrt.exe || goto :fail
    rem CRT image: MSVC-compiled CRT functions may be skipped, none may fail.
    build\ncc-uwcheck.exe %OUT%\p0_crt.exe || goto :fail
) else (
    echo ncc-uwcheck.exe not built; run build.cmd to include this check
)

echo === dumpbin /unwindinfo (for review)
dumpbin /nologo /unwindinfo %OUT%\p0_nocrt.exe > %OUT%\nocrt.unwind.txt
type %OUT%\nocrt.unwind.txt

echo.
echo phase-0 acceptance PASSED
echo Next manual steps: open %OUT%\p0_nocrt.exe in the VS debugger, break on p0_main,
echo step by line through %OUT%\p0_nocrt.obj.pseudo.c and check the call stack; and
echo compare S_FRAMEPROC in "cvdump %OUT%\p0_nocrt.obj" with a cl /Z7 object (Q-26).
exit /b 0

:fail
echo phase-0 acceptance FAILED
exit /b 1
