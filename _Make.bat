@echo off

set CYGWIN=C:\cygwin64\bin

if not exist %CYGWIN%\bash.exe set CYGWIN=C:\msys\1.0\bin

set CHERE_INVOKING=1
%CYGWIN%\bash --login -i -c "cd \"$(cygpath -u '%~dp0')\" && make && rm -f qwark.prx qwark.sym"
