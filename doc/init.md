# Run defcond as a service

The [service examples](../contrib/init/) start the DeFCoN daemon at boot or login. Choose the file for the service manager installed on your machine.

## Linux with systemd

The [systemd unit](../contrib/init/defcond.service) runs as the system account and group `defcon`. Create that account and group, install `defcond` at `/usr/bin/defcond`, and provide a configuration file at `/etc/defcon/defcon.conf`. Make the configuration file and data directory readable only by the `defcon` user and group, because the configuration can hold RPC credentials. The unit stores chain data in `/var/lib/defcond` and the PID file in `/run/defcond/defcond.pid`.

From the repository root, install and start the unit:

```sh
sudo install -m 0644 contrib/init/defcond.service /etc/systemd/system/defcond.service
sudo systemctl daemon-reload
sudo systemctl enable --now defcond.service
systemctl status defcond.service
```

With the default cookie authentication, run the CLI as the service user and point it at the same configuration and data directories:

```sh
sudo -u defcon defcon-cli -conf=/etc/defcon/defcon.conf -datadir=/var/lib/defcond getblockchaininfo
```

Use `systemctl edit defcond.service` for local unit overrides. Command-line arguments in the unit take precedence over matching settings in the [configuration file](defcon-conf.md).

## Other Linux service managers

| Service manager | Example | Configuration |
| --- | --- | --- |
| OpenRC | [init script](../contrib/init/defcond.openrc) | Install as `/etc/init.d/defcond`. Use the [companion file](../contrib/init/defcond.openrcconf) as `/etc/conf.d/defcond`. |
| SysV on CentOS | [init script](../contrib/init/defcond.init) | Install as `/etc/init.d/defcond`. Overrides are read from `/etc/sysconfig/defcond`. The default options include `-disablewallet`. |
| Upstart | [job definition](../contrib/init/defcond.conf) | Install as `/etc/init/defcond.conf`. |

The Upstart example refuses to start without an `rpcpassword` in the configuration. The OpenRC example refuses only when `rpcuser` is set without `rpcpassword`. The daemon itself supports cookie authentication. Check the selected script before enabling it. Legacy scripts use literal environment names such as `BITCOIND_DATADIR` and `BITCOIND_OPTS`; use the names read by your script.

## macOS with launchd

The [launch agent](../contrib/init/org.defcon.defcond.plist) starts `/usr/local/bin/defcond` when the current user logs in. Set the binary path to your installation, then copy the plist to `~/Library/LaunchAgents/`. The default mainnet data directory is `~/Library/Application Support/Defcon/`.
