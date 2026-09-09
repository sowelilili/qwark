#!/bin/sh
cd "$(dirname "$0")" || exit 1
rm -f ./*.sprx ./*.elf ./*.prx ./*.sym
rm -rf objs

make clean
rm -f qwark-host.exe qwark-rpcs3.exe test/qwark-test.exe
rm -rf qwark-host-root qwark-rpcs3-root test/qwark-host-root test/qwark-rpcs3-root
