# Run defcond as a service

These files are examples for packagers and operators. Check the daemon path, service account, configuration file and data directory against the local installation. The [init guide](../../doc/init.md) describes the platform-specific setup.

| Service manager | Example files |
| --- | --- |
| systemd | [defcond.service](defcond.service) |
| OpenRC | [Service](defcond.openrc) and [configuration](defcond.openrcconf) |
| macOS launchd | [org.defcon.defcond.plist](org.defcon.defcond.plist) |
| Upstart | [defcond.conf](defcond.conf) |
| SysV init on CentOS | [defcond.init](defcond.init) |

The systemd example runs `/usr/bin/defcond` with `/etc/defcon/defcon.conf` and stores node data in `/var/lib/defcond`. Some inherited service templates still use variable names beginning with `BITCOIND_`; use the variable names defined by the selected template.
