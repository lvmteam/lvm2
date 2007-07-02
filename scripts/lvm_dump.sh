#!/bin/bash
# we use some bash-isms (getopts?)

# lvm_dump: This script is used to collect pertinent information for
#           the debugging of lvm issues.

# following external commands are used throughout the script
# which, echo and test are internal in bash at least
MKDIR=mkdir # need -p
TAR=tar # need czf
RM=rm # need -rf
CP=cp
TAIL=tail # we need -n
LS=ls # need -la
PS=ps # need alx
SED=sed
DD=dd
CUT=cut
DATE=date
BASENAME=basename
UNAME=uname

# user may override lvm and dmsetup location by setting LVM_BINARY
# and DMSETUP_BINARY respectively
LVM=${LVM_BINARY-lvm}
DMSETUP=${DMSETUP_BINARY-dmsetup}

die() {
    code=$1; shift
    echo "$@" 1>&2
    exit $code
}

# which should error out if the binary is not executable, although i
# am not sure we can rely on this
which $LVM >& /dev/null || die 2 "Fatal: could not find lvm binary '$LVM'"
test -x `which $LVM` || die 2 "Fatal: lvm binary '$LVM' not executable"
which $DMSETUP >& /dev/null || die 2 "Fatal: could not find dmsetup binary '$DMSETUP'"
test -x `which $DMSETUP` || die 2 "Fatal: dmsetup binary '$DMSETUP' not executable"

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

NOW=`$DATE -u +%G%m%d%k%M%S | /usr/bin/tr -d ' '`
if test -n "$userdir"; then
	dir="$userdir"
else
	dirbase="lvmdump-$HOSTNAME-$NOW"
	dir="$HOME/$dirbase"
fi

test -e $dir && die 3 "Fatal: $dir already exists"
$MKDIR -p $dir || die 4 "Fatal: could not create $dir"

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
	log "$LVM vgscan -vvvv > $dir/vgscan 2>&1"

	myecho "  pvscan..."
	log "$LVM pvscan -v >> $dir/pvscan 2>> $log"

	myecho "  lvs..."
	log "$LVM lvs -a -o +devices >> $dir/lvs 2>> $log"

	myecho "  pvs..."
	log "$LVM pvs -a -v > $dir/pvs 2>> $log"

	echo "  vgs..."
	log "$LVM vgs -v > $dir/vgs 2>> $log"
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
$LVM lvs --version >> $dir/versions 2>> $log
echo "DEVICE MAPPER VERSION:" >> $dir/versions
$DMSETUP --version >> $dir/versions 2>> $log
echo "KERNEL VERSION:" >> $dir/versions
$UNAME -a >> $dir/versions 2>> $log
echo "DM TARGETS VERSIONS:" >> $dir/versions
$DMSETUP targets >> $dir/versions 2>> $log

myecho "Gathering dmsetup info..."
log "$DMSETUP info -c > $dir/dmsetup_info 2>> $log"
log "$DMSETUP table > $dir/dmsetup_table 2>> $log"
log "$DMSETUP status > $dir/dmsetup_status 2>> $log"

myecho "Gathering process info..."
log "$PS alx > $dir/ps_info 2>> $log"

myecho "Gathering console messages..."
log "$TAIL -n 75 /var/log/messages > $dir/messages 2>> $log"

myecho "Gathering /etc/lvm info..."
log "$CP -a /etc/lvm $dir/lvm 2>> $log"

myecho "Gathering /dev listing..."
log "$LS -laR /dev > $dir/dev_listing 2>> $log"

myecho "Gathering /sys/block listing..."
log "$LS -laR /sys/block > $dir/sysblock_listing"

if (( $metadata )); then
	myecho "Gathering LVM metadata from Physical Volumes..."

	log "$MKDIR -p $dir/metadata"

	pvs="$($LVM pvs --separator , --noheadings --units s --nosuffix -o \
	    name,pe_start 2>> $log | $SED -e 's/^ *//')"
	for line in "$pvs"
	do
		test -z "$line" && continue
		pv="$(echo $line | $CUT -d, -f1)"
		pe_start="$(echo $line | $CUT -d, -f2)"
		name="$($BASENAME $pv)"
		myecho "  $pv"
		log "$DD if=$pv of=$dir/metadata/$name bs=512 count=$pe_start 2>> $log"
	done
fi

if test -z "$userdir"; then
	lvm_dump="$dirbase.tgz"
	myecho "Creating report tarball in $HOME/$lvm_dump..."
	cd $HOME
	$TAR czf $lvm_dump $dirbase 2>/dev/null
	$RM -rf $dir
fi

if test "$UID" != "0" && test "$EUID" != "0"; then
	myecho
	myecho "WARNING! Running as non-privileged user, dump is likely incomplete!"
	myecho
fi

exit 0

