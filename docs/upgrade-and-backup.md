# Upgrade and backup

Stop every process using the data before copying, restoring, repairing or changing
versions. Journal locks prevent concurrent writers; they do not make a copy of
several changing files consistent. Save the version (`openportd --version`, or
Status → Copy diagnostics), image tag/digest and startup flags with each backup.
Keep the old executable or image for rollback.

## What to save

| Installation | Data root |
| --- | --- |
| Homebrew service | `$(brew --prefix)/var/openport` |
| Docker / Compose | Volume `openport`, mounted at `/var/lib/openport` |
| Linux systemd package | `/var/lib/openport` (systemd may store it under `/var/lib/private/openport`) |
| Native foreground | `~/.openport`; demo journals default to `~/.openport/demo` |

Back up the entire root, including hidden files and subdirectories:

- `paper-journal.jsonl`, `accounts/` and `replays/`: main and named accounts,
  archived attempts and replay runs. Keep journal siblings, equity CSVs, saved
  commands/inputs and metadata together for review and verification.
- `playbooks.json`, `backtests/` and other saved settings/results beside the main
  journal. Deleted or archived account files may still exist.
- `candles/` and `series/`: cached prices and derived volatility history. Docker
  defaults to `.openport/candles` and `.openport/series` inside the volume.
- `recordings/`: recorded feeds. The default is `~/.openport/recordings`, so the
  Homebrew service's recordings are outside `var/openport`. Include explicit
  `--record-dir`, `--candle-dir`, `--series-dir` or journal paths elsewhere too.
- `write-token`, named-token and notification configuration files, and `crash.log`.
  Backups containing tokens grant the same access as the originals. Keep them
  private and encrypt off-machine copies. Recordings remain subject to provider
  redistribution terms.

Save service overrides, environment files, supplied event/dividend calendars and
custom scenarios separately. Environment credentials are not automatically saved
in journals. Browser settings and tab tokens live in the browser. Sandboxes are
disposable and deleted on startup; restoring files does not restore visitor sessions.

## Back up and upgrade

For Homebrew:

```sh
brew services stop openport
umask 077
tar -czf openport-before-upgrade.tgz -C "$(brew --prefix)/var" openport
brew update
brew upgrade openport
brew services start openport
openportd --print-url --open --write-token-file "$(brew --prefix)/var/openport/write-token"
```

A tap added before the project moved, as `38st/openport`, still updates through
GitHub's redirect. Point it at the new home with `brew tap --custom-remote
38st/openport https://github.com/armanruzgar/openport`.

For the repository's Compose file, run these commands beside `docker-compose.yml`.
They use the default `openport` volume; substitute your volume name if customized.
The archive is owner-only; move it to private backup storage afterwards.

```sh
docker compose stop openport
docker compose images
docker run --rm --user 0 --entrypoint sh \
  -v openport:/data:ro -v "$PWD":/backup ghcr.io/armanruzgar/openport \
  -c 'umask 077; tar -czf /backup/openport-before-upgrade.tgz -C /data .'
docker compose pull
docker compose up -d
```

Pin `OPENPORT_IMAGE` to an exact release or digest for repeatable updates.
`docker compose down` preserves data; `down --volumes` deletes it. For a native
or packaged service, stop it (`sudo systemctl stop openport` for the package),
archive the root with ownership and modes intact, then replace the executable.
For systemd, resolve `/var/lib/openport` first: if it is a symlink to
`/var/lib/private/openport`, archive the target directory's contents, not just the
symlink. Using `tar -C REAL_DATA_DIRECTORY ... .` includes hidden contents.
Packages do not automatically start or restart the service: run `sudo systemctl
daemon-reload` and `sudo systemctl start openport` after upgrading.

After restarting, check Status for the expected version and feed, inspect account
history and storage warnings, and test a disposable simulated account. Keep the
backup until satisfied. No data outside market hours does not prove an upgrade failed.

## Restore or roll back

Stop the service. Preserve the newer data separately; restore into an empty
directory or **new volume**, not on top of newer journal files. For Docker:

```sh
docker volume create openport-restored
docker run --rm --user 0 --entrypoint tar \
  -v openport-restored:/data -v "$PWD":/backup:ro ghcr.io/armanruzgar/openport \
  -xzf /backup/openport-before-upgrade.tgz -C /data
```

Set `OPENPORT_VOLUME=openport-restored` and `OPENPORT_IMAGE` to the saved version
when starting Compose. Never run two services against one journal or host port.
Preserve UID 10001 ownership for Docker. Native/Homebrew restores must belong
to the service user. Restore systemd data as root while stopped, into its
`StateDirectory`; systemd applies dynamic-user ownership on start. Preserve
private token/configuration permissions, and reopen the saved token link if needed.

Rollback means the old executable **and its matching pre-upgrade backup**. Data
written after that backup is not part of the rollback; preserve it for inspection.
Do not assume an older executable can read journals written by a newer release.

## Journal compatibility and repair

Journals are append-only JSONL with a verified sequence/hash chain and versioned
trading schema/policy. Current builds accept schema 1/2 snapshots and schema 3
checkpoints/deltas with `index-v1` or `v2` tick policies. Unknown versions and
corrupt records are rejected. Compatibility is checked by code, not implied by
the `.jsonl` extension. There is no general backwards-readability guarantee.

`openportd --compact-journals --paper-journal PATH` rewrites supported older
journals into the current compact format and keeps `FILE.bak`. It is not a
downgrade converter. Keep a full backup first.

For a torn final line after a crash or full disk, stop the server and inspect first:

```sh
openportd --repair-journals --dry-run --paper-journal /path/to/paper-journal.jsonl
openportd --repair-journals --paper-journal /path/to/paper-journal.jsonl
```

This selects the main journal and account/replay journals beside it. Add
`--file /path/to/one.jsonl` to select only that file. Dry-run reports verified
records and bytes to remove without changing files. Repair saves the original
as `FILE.torn-TIME`, then removes only a torn final line; earlier damage is refused.
Exit 0 means inspected/repaired, 1 means a file failed, and 2 means invalid options.
It does not reconstruct lost trades or fix arbitrary edits. Check disk space
first and never hand-edit hashes to make a corrupt journal appear valid.

The local `crash.log` records fatal signals and build identifiers where possible.
It contains no market timestamps or memory dump and cannot record SIGKILL, power
loss or every stack overflow. See [diagnostics](configuration.md#diagnostics).
