# %%{_sysctldir} and %%{_presetdir} are provided by systemd-rpm-macros; fall
# back if they are missing from the build environment so the spec stays
# buildable everywhere.
%{!?_sysctldir: %global _sysctldir /usr/lib/sysctl.d}
%{!?_presetdir: %global _presetdir /usr/lib/systemd/system-preset}

Name:           bkup
Version:        0.10.0
Release:        1%{?dist}
Summary:        Encrypted deduplicating backup tool (CLI + daemon + GUI)

License:        GPL-3.0-or-later
URL:            https://github.com/dhugh100/bkup
Source0:        %{name}-%{version}.tar.gz
# Raises fs.fanotify.max_user_marks for bkupd's recursive directory watches.
Source1:        60-bkupd-fanotify.conf
Source2:        bkupd.service
# Enables bkupd.service when %%systemd_post runs systemctl preset.
Source3:        80-bkupd.preset

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconfig
BuildRequires:  sqlite-devel
BuildRequires:  libzstd-devel
BuildRequires:  libsodium-devel
BuildRequires:  libssh2-devel
BuildRequires:  gtk4-devel
BuildRequires:  selinux-policy-devel
BuildRequires:  systemd-rpm-macros

# Library runtime deps are picked up automatically from the ELF binaries.
# These are only for the %post/%postun SELinux scriptlets.
Requires(post):   policycoreutils
Requires(post):   libselinux-utils
Requires(postun): policycoreutils

%description
Content-addressed, deduplicating, encrypted backup system. Installs the bkup
CLI, the bkupd root daemon, and the bkup-gui restore browser into
/usr/local/bin, plus the bkupd SELinux policy module. The %%post scriptlet
loads the module and runs restorecon so the daemon binary and per-user catalog
paths are labeled automatically -- no manual "make relabel" step.

This package installs the bkupd.service unit and ships a preset that enables it
on install, so the daemon comes back after a reboot. It does not start the
daemon and does not manage /etc/bkup.conf; those remain under administrator
control.

%prep
%setup -q

%build
make all
make -f /usr/share/selinux/devel/Makefile -C selinux bkupd.pp

%install
install -D -m 0755 bin/bkup     %{buildroot}/usr/local/bin/bkup
install -D -m 0755 bin/bkupd    %{buildroot}/usr/local/bin/bkupd
install -D -m 0755 bin/bkup-gui %{buildroot}/usr/local/bin/bkup-gui
install -D -m 0644 selinux/bkupd.pp \
        %{buildroot}%{_datadir}/selinux/packages/%{name}/bkupd.pp
install -D -m 0644 %{SOURCE1} \
        %{buildroot}%{_sysctldir}/60-bkupd-fanotify.conf
install -D -m 0644 %{SOURCE2} \
        %{buildroot}%{_unitdir}/bkupd.service
install -D -m 0644 %{SOURCE3} \
        %{buildroot}%{_presetdir}/80-bkupd.preset
install -D -m 0755 doSetup.sh \
        %{buildroot}/usr/local/bin/bkup-setup

%files
%license LICENSE
/usr/local/bin/bkup
/usr/local/bin/bkupd
/usr/local/bin/bkup-gui
%dir %{_datadir}/selinux/packages/%{name}
%{_datadir}/selinux/packages/%{name}/bkupd.pp
%{_sysctldir}/60-bkupd-fanotify.conf
%{_unitdir}/bkupd.service
%{_presetdir}/80-bkupd.preset
/usr/local/bin/bkup-setup

# Load (install or upgrade) the policy module and relabel the daemon binary plus
# the data/log paths the .fc covers. Runs on both fresh install and upgrade.
%post
%systemd_post bkupd.service
if [ -x /usr/sbin/selinuxenabled ] && /usr/sbin/selinuxenabled; then
    /usr/sbin/semodule -i %{_datadir}/selinux/packages/%{name}/bkupd.pp || :
    /usr/sbin/restorecon -RvF /usr/local/bin/bkupd \
        /home/*/.local/share/bkup /root/.local/share/bkup /var/log/bkup \
        2>/dev/null || :
fi

# Apply the fanotify sysctl now so a fresh install/upgrade doesn't have to wait
# for the next boot. (The drop-in is also applied automatically at boot by
# systemd-sysctl.service.)
/usr/lib/systemd/systemd-sysctl %{_sysctldir}/60-bkupd-fanotify.conf \
    2>/dev/null || :

# Only on a fresh install ($1 == 1). On upgrade ($1 > 1) the repos are already
# initialized and re-running bkup-setup would be a no-op at best, so stay quiet.
if [ "$1" -eq 1 ]; then
echo ""
echo "==> bkup installed. Next steps:"
echo "    1. Create /etc/bkup.conf (see 'man bkup' or config.md)."
echo "    2. Run 'sudo bkup-setup' to set the passphrase and init each user's repo."
echo "    3. Run 'sudo systemctl start bkupd' to start the daemon."
echo "       (It is already enabled for boot by the shipped preset.)"
echo ""
fi

%preun
%systemd_preun bkupd.service

# Remove the module only on full uninstall ($1 == 0), not on upgrade.
%postun
%systemd_postun_with_restart bkupd.service
if [ "$1" -eq 0 ]; then
    if [ -x /usr/sbin/selinuxenabled ] && /usr/sbin/selinuxenabled; then
        /usr/sbin/semodule -r bkupd 2>/dev/null || :
        # Module gone -> bkupd_catalog_t/bkupd_log_t no longer resolve, which
        # would strand these (RPM-unowned) paths as unlabeled_t. Reset them to
        # the policy default now that the module is removed.
        /usr/sbin/restorecon -RF /home/*/.local/share/bkup \
            /root/.local/share/bkup /var/log/bkup 2>/dev/null || :
    fi
fi

%changelog
* Tue Aug 11 2026 dhugh <dhugh100@users.noreply.github.com> - 0.10.0-1
- Release 0.10.0
* Tue Aug 11 2026 dhugh <dhugh100@users.noreply.github.com> - 0.9.0-1
- Release 0.9.0
* Sun Aug 09 2026 dhugh <dhugh100@users.noreply.github.com> - 0.8.0-2
- Ship a systemd preset enabling bkupd.service on install
* Fri Aug 07 2026 dhugh <dhugh100@users.noreply.github.com> - 0.8.0-1
- Release 0.8.0
* Tue Aug 04 2026 dhugh <dhugh100@users.noreply.github.com> - 0.7.2-1
- Release 0.7.2
* Tue Aug 04 2026 dhugh <dhugh100@users.noreply.github.com> - 0.7.1-1
- Release 0.7.1
* Thu Jul 16 2026 dhugh <dhugh100@users.noreply.github.com> - 0.7.0-1
- Release 0.7.0
* Mon Jun 29 2026 dhugh <dhugh100@users.noreply.github.com> - 0.6.0-1
- Release 0.6.0
* Sun Jun 28 2026 dhugh <dhugh100@users.noreply.github.com> - 0.5.10-1
- Release 0.5.10
* Sun Jun 28 2026 dhugh <dhugh100@users.noreply.github.com> - 0.5.9-1
- Release 0.5.9
* Sat Jun 20 2026 dhugh <dhugh100@users.noreply.github.com> - 0.5.8-1
- Release 0.5.8
* Sat Jun 20 2026 dhugh <dhugh100@users.noreply.github.com> - 0.5.7-1
- Release 0.5.7
* Sat Jun 20 2026 dhugh <dhugh100@users.noreply.github.com> - 0.5.6-1
- Release 0.5.6
* Sat Jun 20 2026 dhugh <dhugh100@users.noreply.github.com> - 0.5.5-1
- Release 0.5.5
* Sat Jun 20 2026 dhugh <dhugh100@users.noreply.github.com> - 0.5.4-1
- Release 0.5.4
* Sat Jun 20 2026 dhugh <dhugh100@users.noreply.github.com> - 0.5.3-1
- Release 0.5.3
* Sat Jun 20 2026 dhugh <dhugh100@users.noreply.github.com> - 0.5.2-1
- Release 0.5.2
* Sat Jun 20 2026 dhugh <dhugh100@users.noreply.github.com> - 0.5.1-1
- Release 0.5.1
* Sat Jun 20 2026 dhugh <dhugh100@users.noreply.github.com> - 0.5.0-1
- Release 0.5.0
* Wed Jun 17 2026 dhugh <dhugh100@users.noreply.github.com> - 0.4.0-1
- Release 0.4.0
* Wed Jun 17 2026 dhugh <dhugh100@users.noreply.github.com> - 0.3.7-1
- Release 0.3.7
* Tue Jun 16 2026 dhugh <dhugh100@users.noreply.github.com> - 0.3.6-1
- Release 0.3.6
* Tue Jun 16 2026 dhugh <dhugh100@users.noreply.github.com> - 0.3.5-1
- Release 0.3.5
* Tue Jun 16 2026 dhugh <dhugh100@users.noreply.github.com> - 0.3.4-1
- Release 0.3.4
* Tue Jun 16 2026 dhugh <dhugh100@users.noreply.github.com> - 0.3.3-1
- Release 0.3.3
* Tue Jun 16 2026 dhugh <dhugh100@users.noreply.github.com> - 0.3.2-1
- Release 0.3.2
* Tue Jun 16 2026 dhugh <dhugh100@users.noreply.github.com> - 0.3.1-1
- Release 0.3.1
* Tue Jun 16 2026 dhugh <dhugh100@users.noreply.github.com> - 0.3.0-1
- Release 0.3.0
* Tue Jun 16 2026 dhugh <dhugh100@users.noreply.github.com> - 0.2.1-1
- Release 0.2.1
* Tue Jun 16 2026 dhugh <dhugh100@users.noreply.github.com> - 0.2.0-1
- Release 0.2.0
* Tue Jun 16 2026 dhugh <dhugh100@users.noreply.github.com> - 0.1.0-1
- Release 0.1.0
* Tue Jun 16 2026 dhugh <dhugh100@users.noreply.github.com> - 1.0.9-1
- Initial RPM: install bkup, bkupd, bkup-gui to /usr/local/bin
- Ship bkupd SELinux module; auto semodule + restorecon in %%post
- Policy bumped to 1.0.9 to label /usr/local/bin/bkupd as bkupd_exec_t
