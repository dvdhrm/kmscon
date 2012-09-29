#!/bin/sh
set -e
mkdir -p m4
autoreconf -i
./configure --enable-debug "$@"
