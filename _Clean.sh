#!/bin/sh
cd "$(dirname "$0")" || exit 1
rm -f ./*.sprx ./*.elf ./*.prx ./*.sym
rm -rf objs

make clean
rm -f qwark-host.exe test/qwark-test.exe
rm -rf qwark-host-root test/qwark-host-root
