#!/bin/sh
mkdir -p m4/
autoreconf -i
./configure --enable-debug $*
