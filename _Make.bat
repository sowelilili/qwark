@echo off

rem A test build by default. Arguments go to make: "_Make.bat QWARK_DEV=0" builds
rem a release module, and "_Make.bat release" a release module and dist (makefile).

set CYGWIN=C:\cygwin64\bin

if not exist %CYGWIN%\bash.exe set CYGWIN=C:\msys\1.0\bin

set CHERE_INVOKING=1
%CYGWIN%\bash --login -i -c "cd \"$(cygpath -u '%~dp0')\" && make %* && rm -f qwark.prx qwark.sym"
