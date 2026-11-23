@echo off
rem build.cmd — Windows build of the phase-0 tools with clang (stage 0, see
rem Design/13-self-hosting-bootstrap.md). Needs clang on PATH: either LLVM for
rem Windows or the "C++ Clang tools for Windows" component of VS 2026 (run
rem from a Native Tools Command Prompt so the UCRT headers and libs are found).
setlocal enabledelayedexpansion
set CFLAGS=-std=c2x -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -D_CRT_SECURE_NO_WARNINGS
if not exist build mkdir build
rem cmd does not expand wildcards for clang, so collect the file list here.
set LIB_SRC=
for %%f in (src\core\*.c src\target\arm64\*.c src\obj\coff\*.c src\debug\codeview\*.c) do set LIB_SRC=!LIB_SRC! %%f
set TEST_SRC=
for %%f in (tests\unit\*.c) do set TEST_SRC=!TEST_SRC! %%f
clang %CFLAGS% %LIB_SRC% src\tools\p0demo.c -o build\p0demo.exe || exit /b 1
clang %CFLAGS% %LIB_SRC% %TEST_SRC% -o build\ncc-test.exe || exit /b 1
echo built build\p0demo.exe and build\ncc-test.exe
