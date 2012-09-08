#!/bin/sh
mkdir -p m4/ || exit 1
autoreconf -i || exit 1
./configure --enable-debug $* || exit 1
