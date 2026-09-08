#!/bin/sh
cd "$(dirname "$0")" || exit 1
make || exit $?
rm -f qwark.prx qwark.sym
