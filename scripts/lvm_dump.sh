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
	echo "    -d dump directory to place data in (default=/tmp/lvm_dump.\$\$)"
	echo "    -c if running clvmd, gather cluster data as well"
	echo ""
	
	exit 1
}

advanced=0
clustered=0
metadata=0
while getopts :acd:hm opt; do
	case $opt in 
		a)	advanced=1 ;;
		c)	clustered=1 ;;
		d)	lvm_dir=$OPTARG ;;
		h)	usage ;;
		m)	metadata=1 ;;
		:)	echo "$0: $OPTARG requires a value:"; usage ;;
		\?)     echo "$0: unknown option $OPTARG"; usage ;;
		*)	usage ;;
	esac
done

dir=`mktemp -d -p /tmp lvm_dump.XXXXXX` || exit 2
lvm_dir="$dir/lvm_dump"

echo " "
echo "Creating dump directory: $lvm_dir"
echo " "

mkdir -p $lvm_dir || exit 3

if (( $advanced )); then
	echo "Gathering LVM volume info..."

	echo "  vgscan..."
	vgscan -vvvv > $lvm_dir/vgscan 2>&1

	echo "  pvscan..."
	pvscan -v >> $lvm_dir/pvscan 2>/dev/null

	echo "  lvs..."
	lvs -a -o +devices >> $lvm_dir/lvs 2>/dev/null

	echo "  pvs..."
	pvs -a -v > $lvm_dir/pvs 2>/dev/null

	echo "  vgs..."
	vgs -v > $lvm_dir/vgs 2>/dev/null
fi

if (( $clustered )); then
	echo "Gathering cluster info..."
	echo "STATUS: " > $lvm_dir/cluster_info
	echo "----------------------------------" >> $lvm_dir/cluster_info
	cman_tool status >> $lvm_dir/cluster_info
	echo " " >> $lvm_dir/lvm_info

	echo "SERVICES: " >> $lvm_dir/cluster_info
	echo "----------------------------------" >> $lvm_dir/cluster_info
	cman_tool services >> $lvm_dir/cluster_info
	echo " " >> $lvm_dir/lvm_info
fi

echo "Gathering LVM & device-mapper version info..."
echo "LVM VERSION:" > $lvm_dir/versions
lvs --version >> $lvm_dir/versions
echo "DEVICE MAPPER VERSION:" >> $lvm_dir/versions
dmsetup --version >> $lvm_dir/versions

echo "Gathering dmsetup info..."
dmsetup info -c > $lvm_dir/dmsetup_info
dmsetup table > $lvm_dir/dmsetup_table
dmsetup status > $lvm_dir/dmsetup_status

echo "Gathering process info..."
ps alx > $lvm_dir/ps_info

echo "Gathering console messages..."
tail -n 75 /var/log/messages > $lvm_dir/messages

echo "Gathering /etc/lvm info..."
cp -a /etc/lvm $lvm_dir/lvm

echo "Gathering /dev listing..."
ls -la /dev > $lvm_dir/dev_listing

if (( $metadata )); then
	echo "Gathering LVM metadata from Physical Volumes..."

	mkdir -p $lvm_dir/metadata

	for pv in `pvs --noheadings -o name`
	do
		echo "  $pv"
		name=`basename $pv`
		dd if=$pv of=$lvm_dir/metadata/$name bs=512 count=`pvs --noheadings --nosuffix --units s -o pe_start $pv | tr -d \ `
	done 2>/dev/null
fi

lvm_dump=$lvm_dir.tgz
echo "Creating tarball $lvm_dump..."
tar czf $lvm_dump $lvm_dir 2>/dev/null

exit 0

