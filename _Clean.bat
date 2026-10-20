@echo off

set CYGWIN=C:\cygwin64\bin
set CHERE_INVOKING=1

if not exist %CYGWIN%\bash.exe set CYGWIN=C:\msys\1.0\bin

pushd "%~dp0"

if exist *.sprx del *.sprx>nul
if exist *.elf  del *.elf>nul
if exist *.prx  del *.prx>nul
if exist *.sym  del *.sym>nul
if exist *.pkg  del *.pkg>nul
if exist objs   rmdir /s /q objs>nul
if exist installer\build rmdir /s /q installer\build>nul
if exist installer\objs  rmdir /s /q installer\objs>nul
if exist installer\installer.elf  del installer\installer.elf>nul
if exist installer\installer.self del installer\installer.self>nul

%CYGWIN%\bash --login -i -c "cd \"$(cygpath -u '%~dp0')\" && make clean">nul

popd
