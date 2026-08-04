#!/bin/bash
# First-time bkup setup: write /etc/bkup.key and run bkup init for each
# [user "X"] section found in /etc/bkup.conf.
set -e

KEY=/etc/bkup.key
CONF=/etc/bkup.conf

if [ "$EUID" -ne 0 ]; then
    echo "Run as root: sudo bkup-setup"
    exit 1
fi

if [ -f "$KEY" ]; then
    echo "$KEY already exists. Remove it first to reset the passphrase."
    exit 1
fi

if [ ! -f "$CONF" ]; then
    echo "$CONF not found. Create it first (see /usr/share/doc/bkup/config.md)."
    exit 1
fi

# Prompt for passphrase with confirmation
while true; do
    read -rsp "Passphrase for bkup repos: " pass1; echo
    read -rsp "Confirm passphrase: " pass2; echo
    [ "$pass1" = "$pass2" ] && break
    echo "Passphrases do not match. Try again."
done

printf '%s\n' "$pass1" > "$KEY"
chmod 600 "$KEY"
chown root:root "$KEY"
echo "Wrote $KEY (mode 600)."

# Init each [user "X"] section found in the config
users=$(awk -F'"' '/^\[user "/{print $2}' "$CONF")
if [ -z "$users" ]; then
    echo "No [user ...] sections found in $CONF."
    echo "Add them, then run: bkup -c $CONF -U USERNAME init"
    exit 0
fi

for u in $users; do
    echo "Running: bkup -c $CONF -U $u init"
    /usr/local/bin/bkup -c "$CONF" -U "$u" init
    # The catalog dir was just created; relabel it so bkupd_t can write it.
    # (%post restorecon ran at install time before this dir existed.)
    if [ -x /usr/sbin/selinuxenabled ] && /usr/sbin/selinuxenabled; then
        home=$(getent passwd "$u" | cut -d: -f6 2>/dev/null)
        if [ -n "$home" ] && [ -d "$home/.local/share/bkup" ]; then
            /usr/sbin/restorecon -RF "$home/.local/share/bkup" 2>/dev/null || :
        fi
    fi
done

echo ""
echo "Setup complete. As root, enable and start the daemon:"
echo "  systemctl enable --now bkupd"
