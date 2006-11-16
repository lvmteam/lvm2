#!/bin/bash
#
# lvm_dump: This script is used to collect pertinent information for
#           the debugging of lvm issues.
#

function usage {
	echo "$0 [options]"
	echo "    -h print this message"
	echo "    -a advanced collection - warning: if lvm is already hung,"
	echo "       then this script may hang as well if -a is used"
	echo "    -m gather LVM metadata from the PVs"
	echo "    -d <directory> dump into a directory instead of tarball"
	echo "    -c if running clvmd, gather cluster data as well"
	echo ""
	
	exit 1
}

advanced=0
clustered=0
metadata=0
while getopts :acd:hm opt; do
	case $opt in 
		s)      sysreport=1 ;;
		a)	advanced=1 ;;
		c)	clustered=1 ;;
		d)	userdir=$OPTARG ;;
		h)	usage ;;
		m)	metadata=1 ;;
		:)	echo "$0: $OPTARG requires a value:"; usage ;;
		\?)     echo "$0: unknown option $OPTARG"; usage ;;
		*)	usage ;;
	esac
done

DATE=`/bin/date -u +%G%m%d%k%M%S | /usr/bin/tr -d ' '`
if test -n "$userdir"; then
	dir="$userdir"
else
	dirbase="lvmdump-$HOSTNAME-$DATE"
	dir="$HOME/$dirbase"
fi

if test -e $dir; then
	echo $dir already exists, aborting >&2
	exit 2
fi

if ! mkdir -p $dir; then
	echo Could not create $dir >&2
	exit 3
fi

log="$dir/lvmdump.log"

myecho() {
	echo "$@"
	echo "$@" >> $log
}

log() {
	echo "$@" >> $log
	eval "$@"
}

echo " "
myecho "Creating dump directory: $dir"
echo " "

if (( $advanced )); then
	myecho "Gathering LVM volume info..."

	myecho "  vgscan..."
	log "vgscan -vvvv > $dir/vgscan 2>&1"

	myecho "  pvscan..."
	log "pvscan -v >> $dir/pvscan 2>> $log"

	myecho "  lvs..."
	log "lvs -a -o +devices >> $dir/lvs 2>> $log"

	myecho "  pvs..."
	log "pvs -a -v > $dir/pvs 2>> $log"

	echo "  vgs..."
	log "vgs -v > $dir/vgs 2>> $log"
fi

if (( $clustered )); then
	myecho "Gathering cluster info..."
	echo "STATUS: " > $dir/cluster_info
	echo "----------------------------------" >> $dir/cluster_info
	log "cman_tool status >> $dir/cluster_info 2>> $log"
	echo " " >> $dir/lvm_info

	echo "SERVICES: " >> $dir/cluster_info
	echo "----------------------------------" >> $dir/cluster_info
	log "cman_tool services >> $dir/cluster_info 2>> $log"
	echo " " >> $dir/lvm_info
fi

myecho "Gathering LVM & device-mapper version info..."
echo "LVM VERSION:" > $dir/versions
lvs --version >> $dir/versions 2>> $log
echo "DEVICE MAPPER VERSION:" >> $dir/versions
dmsetup --version >> $dir/versions 2>> $log

myecho "Gathering dmsetup info..."
log "dmsetup info -c > $dir/dmsetup_info 2>> $log"
log "dmsetup table > $dir/dmsetup_table 2>> $log"
log "dmsetup status > $dir/dmsetup_status 2>> $log"

myecho "Gathering process info..."
log "ps alx > $dir/ps_info 2>> $log"

myecho "Gathering console messages..."
log "tail -n 75 /var/log/messages > $dir/messages 2>> $log"

myecho "Gathering /etc/lvm info..."
log "cp -a /etc/lvm $dir/lvm 2>> $log"

myecho "Gathering /dev listing..."
log "ls -la /dev > $dir/dev_listing 2>> $log"

if (( $metadata )); then
	myecho "Gathering LVM metadata from Physical Volumes..."

	log "mkdir -p $dir/metadata"

	pvs="$(pvs --separator , --noheadings --units s --nosuffix -o name,pe_start 2>> $log | \
		sed -e 's/^ *//')"
	for line in "$pvs"
	do
		pv="$(echo $line | cut -d, -f1)"
		pe_start="$(echo $line | cut -d, -f2)"
		name="$(basename $pv)"
		myecho "  $pv"
		log "dd if=$pv of=$dir/metadata/$name bs=512 count=$pe_start 2>> $log"
	done
fi

if test -z "$userdir"; then
	lvm_dump="$dirbase.tgz"
	myecho "Creating report tarball in $HOME/$lvm_dump..."
	cd $HOME
	tar czf $lvm_dump $dirbase 2>/dev/null
	rm -rf $dir
fi

exit 0

