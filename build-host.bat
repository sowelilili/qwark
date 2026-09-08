@echo off
rem Builds qwark-host.exe through build-host.sh in Git Bash.

set BASH=C:\Program Files\Git\bin\bash.exe
if not exist "%BASH%" set BASH=C:\Program Files (x86)\Git\bin\bash.exe

"%BASH%" -lc "cd \"$(cygpath -u '%~dp0')\" && ./build-host.sh"
