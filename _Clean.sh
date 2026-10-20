#!/bin/sh
cd "$(dirname "$0")" || exit 1
rm -f ./*.sprx ./*.elf ./*.prx ./*.sym ./ui.pak ./qwark-installer.pkg
rm -rf objs installer/build installer/objs
rm -f installer/installer.elf installer/installer.self installer/test/installer-test.exe
rm -rf installer/test/root

make clean
rm -f qwark-host.exe qwark-rpcs3.exe test/qwark-test.exe
rm -rf qwark-host-root qwark-rpcs3-root test/qwark-host-root test/qwark-rpcs3-root
