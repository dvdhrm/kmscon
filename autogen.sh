#!/bin/sh
mkdir -p m4/
autoreconf -i
./configure --enable-debug --enable-pango --enable-gles2 $*
