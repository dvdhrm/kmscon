#!/bin/sh
set -e
mkdir -p m4
autoreconf -i

if test ! "x$NOCONFIGURE" = "x1" ; then
    exec ./configure "$@"
fi
