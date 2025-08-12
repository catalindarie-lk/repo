@echo off
setlocal

REM Get workspace folder from argument
set workspace=%1

REM Get target source name: udp_client or udp_server
set target=%2

REM Common source files (in src)
set common=%workspace%\src\resources.c ^
%workspace%\src\mem_pool.c ^
%workspace%\src\sha256.c ^
%workspace%\src\bitmap.c ^
%workspace%\src\queue.c ^
%workspace%\src\hash.c ^
%workspace%\src\checksum.c ^
%workspace%\src\fileio.c ^
%workspace%\src\folders.c

REM Conditional dependency only for udp_server
if "%target%"=="test_server" (
    set private=%workspace%\src\file_handler.c ^
%workspace%\src\message_handler.c ^
%workspace%\src\server_frames.c ^
%workspace%\src\server_statistics.c
) else if "%target%"=="test_client" (
    set private=%workspace%\src\client_api.c ^
%workspace%\src\client_frames.c ^
%workspace%\src\client_statistics.c
) else (
    echo [ERROR] Unknown target: %target%
    exit /b 1
)

REM Initialize MSVC environment (recommended)
REM call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

REM Build with cl.exe
REM "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe" /EHsc /favor:AMD64 ^
"cl.exe" /EHsc /favor:AMD64 ^
%common% ^
%private% ^
%workspace%\%target%.c ^
/I"%workspace%" ^
/O2 ^
/Fe"%workspace%\bin\%target%.exe" ^
/Fo"%workspace%\bin\obj\\" ^
/Fd"%workspace%\bin\pdb\%target%_compiler.pdb" ^
/link user32.lib gdi32.lib comctl32.lib Ws2_32.lib iphlpapi.lib ^
/MACHINE:X64 ^
/PDB:"%workspace%\bin\pdb\%target%_linker.pdb" ^
/ILK:"%workspace%\bin\pdb\%target%.ilk"


REM /Zi ^
REM /Od ^

endlocal