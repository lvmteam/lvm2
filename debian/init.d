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
	modprobe dm-mod >/dev/null 2>&1 && CONT=1

	if test "$CONT"; then
		vgchange -a y 2>/dev/null
		# TODO: attempt to mount all lvm devices; mount -a?
		echo "$NAME."
	else
		echo "device-mapper kernel module not loaded; refusing to init LVM"
	fi
	;;
  stop)
	echo -n "Shutting down $DESC: "
	# TODO: attempt to umount all lvm devices; umount -a?
	vgchange -a n 2>/dev/null && CONT=1

	if test "$CONT"; then
		rmmod dm-mod >/dev/null 2>&1
	fi
	echo "$NAME."
	;;
  restart|force-reload)
	echo -n "Restarting $DESC: "
	# TODO: attempt to umount all lvm devices; umount -a?
	vgchange -a n 2>/dev/null && CONT=1

	if test "$CONT"; then
		rmmod dm-mod >/dev/null 2>&1
		sleep 1
		modprobe dm-mod >/dev/null 2>&1
		vgchange -a y 2>/dev/null
		# TODO: attempt to mount all lvm devices; mount -a?

	fi
	echo "$NAME."
	;;
  *)
	N=/etc/init.d/$NAME
	echo "Usage: $N {start|stop|restart|force-reload}" >&2
	exit 1
	;;
esac

exit 0
