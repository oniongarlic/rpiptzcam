#!/bin/sh
set -x
libtoolize --automake --copy --force
aclocal
mkdir m4
autoconf --force
autoheader --force
automake --add-missing --copy --force-missing --foreign
