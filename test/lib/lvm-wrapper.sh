#!/bin/sh

. lib/paths
cmd=$(echo ./$0|sed "s,.*/,,")

test "$cmd" = lvm && exec "$abs_top_builddir/tools/lvm" "$@"
exec "$abs_top_builddir/tools/lvm" "$cmd" "$@"
