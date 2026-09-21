#!/bin/bash
# vm-snap.sh -- pre/post-backup hook for libvirt/KVM guests with qcow2 disks.
#
#   pre-backup  = /usr/local/bin/vm-snap.sh begin [DOMAIN...]
#   post-backup = /usr/local/bin/vm-snap.sh end   [DOMAIN...]
#
# With no DOMAIN arguments every domain known to the hypervisor is handled.
#
# begin: for each running guest, take an external disk-only snapshot so the
#        guest keeps writing to a fresh *.bkup-overlay file and the base qcow2
#        stops changing for the duration of the backup. Quiesced through the
#        guest agent when one answers, crash-consistent otherwise. Also stages
#        what a disk alone cannot restore: the domain XML (with secrets) and
#        the UEFI NVRAM, under $VMSNAP_STAGE.
# end:   fold every overlay back into its base (blockcommit --pivot) and delete
#        it. Runs whether the backup succeeded or not, and begin runs it first
#        too, so an overlay left behind by a crash is folded on the next run
#        instead of stacking another on top.
#
# The backup itself is the normal Bkup scan of the images directory plus the
# staging directory; exclude '*.bkup-overlay' so the live overlay is not read.
# Everything here runs as root inside bkupd (or as whoever runs `bkup backup`).
#
# Restore is deliberately manual: restore the .qcow2 and .xml, put the disk
# back at the path the XML names, `virsh define DOMAIN.xml`, copy the NVRAM
# back if the guest is UEFI.

set -u

URI=${VMSNAP_URI:-qemu:///system}
STAGE=${VMSNAP_STAGE:-/var/lib/bkup/vm}
POOL=${VMSNAP_POOL:-default}        # storage pool that holds the images dir
SUFFIX=.bkup-overlay

v() { virsh -c "$URI" "$@"; }
log() { echo "vm-snap: $*"; }

# Print "type device target source" for every disk of a domain, one per line.
# The source column may contain spaces, so it is everything after the target.
disks() {
    v domblklist --details "$1" | awk 'NR > 2 && NF >= 3 {
        s = ""; for (i = 4; i <= NF; i++) s = s (i > 4 ? " " : "") $i;
        print $1, $2, $3, s }'
}

running() {
    case "$(v domstate "$1" 2>/dev/null)" in
        running|paused) return 0 ;;
        *) return 1 ;;
    esac
}

# Fold and remove any overlay a domain is still using. Idempotent.
end_one() {
    local dom=$1 rc=0 type dev target src
    while read -r type dev target src; do
        [ "$dev" = disk ] || continue
        case "$src" in *"$SUFFIX") ;; *) continue ;; esac
        if ! running "$dom"; then
            log "ERROR: '$dom' is shut off but its disk $target still points at"
            log "  $src -- fold it by hand: qemu-img commit -d '$src', then"
            log "  virsh edit '$dom' to point $target back at the base image"
            rc=1; continue
        fi
        log "$dom: committing $target overlay back into its base"
        if ! v blockcommit "$dom" "$target" --active --pivot --wait >/dev/null; then
            log "ERROR: blockcommit failed for '$dom' $target; overlay left in place"
            rc=1; continue
        fi
        # Delete through libvirt (it owns the images dir) rather than rm.
        v pool-refresh "$POOL" >/dev/null 2>&1
        if ! v vol-delete --pool "$POOL" "$(basename "$src")" >/dev/null; then
            log "WARNING: could not delete overlay '$src'; remove it by hand"
            rc=1
        fi
    done < <(disks "$dom")
    return $rc
}

# Stage XML + NVRAM and, if the guest is running, snapshot its disks.
begin_one() {
    local dom=$1 type dev target src specs=() nvram
    v dumpxml --inactive --security-info "$dom" > "$STAGE/$dom.xml" || {
        log "ERROR: dumpxml failed for '$dom'"; return 1; }
    nvram=$(sed -n 's/.*<nvram[^>]*>\(.*\)<\/nvram>.*/\1/p' "$STAGE/$dom.xml" | head -1)
    if [ -n "$nvram" ] && [ -f "$nvram" ]; then
        # Plain copy: preserving qemu's ownership/times would need CAP_FOWNER,
        # which the confined daemon does not have (and the copy does not need).
        cp -f "$nvram" "$STAGE/$dom.nvram" || {
            log "ERROR: could not copy NVRAM '$nvram' for '$dom'"; return 1; }
    fi

    if ! running "$dom"; then
        log "$dom: shut off; base image is already consistent"
        return 0
    fi

    while read -r type dev target src; do
        if [ "$dev" = disk ] && [ -n "$src" ]; then
            case "$src" in
                *"$SUFFIX") log "ERROR: '$dom' $target is still on an overlay"; return 1 ;;
            esac
            specs+=(--diskspec "$target,file=$src$SUFFIX")
        else
            specs+=(--diskspec "$target,snapshot=no")     # cdrom, floppy, empty
        fi
    done < <(disks "$dom")
    [ ${#specs[@]} -gt 0 ] || { log "$dom: no disks"; return 0; }

    local common=(snapshot-create-as "$dom" --name bkup --disk-only --atomic
                  --no-metadata "${specs[@]}")
    if v "${common[@]}" --quiesce >/dev/null 2>&1; then
        log "$dom: snapshotted (quiesced via guest agent)"
    elif v "${common[@]}" >/dev/null; then
        log "$dom: snapshotted (no guest agent; crash-consistent)"
    else
        log "ERROR: snapshot failed for '$dom'"
        return 1
    fi
    return 0
}

main() {
    local mode=${1:-}; shift || true
    local doms=("$@") dom rc=0
    if [ ${#doms[@]} -eq 0 ]; then
        mapfile -t doms < <(v list --all --name | sed '/^$/d')
    fi
    case "$mode" in
        begin)
            mkdir -p "$STAGE" || { log "ERROR: cannot create $STAGE"; return 1; }
            chmod 700 "$STAGE"      # dumpxml --security-info includes secrets
            for dom in "${doms[@]}"; do end_one "$dom" || rc=1; done
            [ $rc -eq 0 ] || { log "ERROR: stale overlays could not be folded; refusing to start"; return 1; }
            for dom in "${doms[@]}"; do begin_one "$dom" || rc=1; done
            ;;
        end)
            for dom in "${doms[@]}"; do end_one "$dom" || rc=1; done
            ;;
        *)
            echo "usage: $0 begin|end [DOMAIN...]" >&2
            return 2
            ;;
    esac
    return $rc
}

main "$@"
