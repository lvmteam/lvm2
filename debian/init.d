#! /bin/sh
#
# lvm2		This script handles LVM2 initialization/shutdown.
#
#		Written by Andres Salomon <dilinger@mp3revolution.net>.
#

PATH=/sbin:/bin:/usr/sbin:/usr/bin
NAME=lvm2
DESC=LVM

test -f $DAEMON || exit 0

set -e

case "$1" in
  start)
	echo -n "Initializing $DESC: "
	modprobe dm-mod >/dev/null 2>&1
	vgchange -a y 2>/dev/null
	# TODO: attempt to mount all lvm devices; mount -a?
	echo "$NAME."
	;;
  stop)
	echo -n "Shutting down $DESC: "
	# TODO: attempt to umount all lvm devices; umount -a?
	vgchange -a n 2>/dev/null
	rmmod dm-mod >/dev/null 2>&1
	echo "$NAME."
	;;
  restart|force-reload)
	echo -n "Restarting $DESC: "
	stop
	sleep 1
	start
	echo "$NAME."
	;;
  *)
	N=/etc/init.d/$NAME
	echo "Usage: $N {start|stop|restart|force-reload}" >&2
	exit 1
	;;
esac

exit 0
