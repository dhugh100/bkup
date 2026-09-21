#!/bin/bash
# vm-restore.sh -- recover a libvirt/KVM guest from the "vm" backup set.
#
#   vm-restore.sh [-s SNAP | -A EPOCH] [-U USER] [-f] DOMAIN
#
# Restores the domain XML from the staging dir, reads the disk and NVRAM paths
# out of it, restores those, puts them back where the XML expects them (with
# their SELinux labels), and `virsh define`s the domain. It does not start the
# guest; do that from virt-manager or `virsh start`.
#
# Refuses to overwrite an image that is already in place unless -f is given,
# and refuses to touch a running guest. Everything is restored to a scratch
# dir first, so a failed restore leaves the pool untouched. Run as root.

set -eu

URI=${VMSNAP_URI:-qemu:///system}
STAGE=${VMSNAP_STAGE:-/var/lib/bkup/vm}
SCRATCH=${VMRESTORE_DIR:-/root/vmrestore}
BKUP_USER=vm
SNAPARGS=()
FORCE=0

usage() { echo "usage: $0 [-s SNAP | -A EPOCH] [-U USER] [-f] DOMAIN" >&2; exit 2; }

while getopts "s:A:U:f" opt; do
    case $opt in
        s) SNAPARGS=(-s "$OPTARG") ;;
        A) SNAPARGS=(-A "$OPTARG") ;;
        U) BKUP_USER=$OPTARG ;;
        f) FORCE=1 ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))
[ $# -eq 1 ] || usage
DOM=$1

[ "$(id -u)" -eq 0 ] || { echo "run as root" >&2; exit 1; }

v() { virsh -c "$URI" "$@"; }
log() { echo "vm-restore: $*"; }
restore() { bkup -U "$BKUP_USER" restore "${SNAPARGS[@]}" "$@"; }

case "$(v domstate "$DOM" 2>/dev/null || true)" in
    running|paused) log "ERROR: '$DOM' is running; shut it down first"; exit 1 ;;
esac

WORK="$SCRATCH/$DOM"
mkdir -p "$WORK"
chmod 700 "$SCRATCH" "$WORK"

# 1. the definition, which tells us what else to restore
log "restoring definition"
restore -f "$STAGE/$DOM.xml" "$WORK"
XML="$WORK/$DOM.xml"
[ -s "$XML" ] || { log "ERROR: no XML restored for '$DOM'"; exit 1; }

mapfile -t DISKS < <(xmllint --xpath "//devices/disk[@device='disk']/source/@file" "$XML" \
                     | sed -n "s/^ *file=\"\(.*\)\"$/\1/p")
NVRAM=$(xmllint --xpath "string(//os/nvram)" "$XML")
[ ${#DISKS[@]} -gt 0 ] || { log "ERROR: no file-backed disks in $XML"; exit 1; }

# 2. refuse to clobber unless told to
for d in "${DISKS[@]}"; do
    if [ -e "$d" ] && [ $FORCE -eq 0 ]; then
        log "ERROR: $d exists; re-run with -f to overwrite it"; exit 1
    fi
done

# 3. the disks and NVRAM, into the scratch dir
ARGS=()
for d in "${DISKS[@]}"; do ARGS+=(-f "$d"); done
[ -n "$NVRAM" ] && ARGS+=(-f "$STAGE/$DOM.nvram")
log "restoring ${#DISKS[@]} disk(s)${NVRAM:+ and NVRAM}"
restore "${ARGS[@]}" "$WORK"

# 4. into place, labeled for libvirt
for d in "${DISKS[@]}"; do
    src="$WORK/$(basename "$d")"
    [ -s "$src" ] || { log "ERROR: $src was not restored"; exit 1; }
    qemu-img check -q "$src" || { log "ERROR: $src fails qemu-img check"; exit 1; }
    mkdir -p "$(dirname "$d")"
    mv -f "$src" "$d"
    restorecon "$d"
    log "restored $d"
done
if [ -n "$NVRAM" ]; then
    src="$WORK/$DOM.nvram"
    [ -s "$src" ] || { log "ERROR: NVRAM was not restored"; exit 1; }
    mkdir -p "$(dirname "$NVRAM")"
    cp -f "$src" "$NVRAM"
    chown qemu:qemu "$NVRAM"
    restorecon "$NVRAM"
    log "restored $NVRAM"
fi

# 5. register it
v define "$XML"
log "'$DOM' defined; start it with: virsh -c $URI start '$DOM' (or from virt-manager)"
