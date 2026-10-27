# Installation

OpenPort is one `openportd` process that serves the web terminal on port 8080.
Install it with Homebrew on macOS, with Docker, from a release
archive or from source. [Configuration](configuration.md) lists the providers and
every flag.

## Homebrew

On a Mac, with Homebrew:

```bash
brew tap armanruzgar/openport https://github.com/armanruzgar/openport
brew install openport
brew services start openport
openportd --print-url --open --write-token-file "$(brew --prefix)/var/openport/write-token"
```

The last command reads the existing token and opens its link. If startup has not
created the file yet, wait a few seconds and retry. Omit `--open` to print it only.
The service keeps accounts and chart history under
`$(brew --prefix)/var/openport`; logs are in `var/log/openport.log` and
`var/log/openport.error.log`. Apple Silicon installs the self-contained archive;
Intel Macs build the release tag with Homebrew's CMake, Node, Boost, OpenSSL and
zstd dependencies. This takes longer and needs network access to fetch build
dependencies. No Intel bottle is claimed.

For a foreground session, run `openportd --open`. Browser launching is opt-in;
services, SSH and headless runs never open a browser by default. `--open` creates
a private `write-token` beside the journal unless a token is already configured.
Later, `openportd --print-url --open` reopens that link and exits. Supply the same
`--provider demo`, `--paper-journal`, `--write-token-file`, `--address` and `--port`
settings when they differ from defaults. There is no separate daemon config file
or service discovery: the helper uses those flags and `OPENPORT_WRITE_TOKEN`,
with `--write-token` taking precedence. It never creates a missing token or starts
another feed. A failed browser launch leaves the link available for manual use.
Linux browser launching uses `xdg-open` from `xdg-utils`; on a headless server,
print the link and open it on your desktop instead. `--open` hands the token link
to `open` or `xdg-open` as an argument, so other local users can see it in `ps`
while the launcher runs; some `xdg-open` backends run as long as the browser, and
openportd warns on Linux. On a shared machine, run `--print-url` and paste the link.
With only named tokens configured, `--open` opens the terminal without choosing a
token or creating an administrator credential; enter the appropriate token in
the terminal. `--print-url` requires a legacy token setting in that case.

## Docker

With Docker (Cboe delayed SPX, SPY, QQQ, IWM and DIA, no key needed), from the
published image for amd64 and arm64:

```bash
docker run --rm --name openport -p 127.0.0.1:8080:8080 -v openport:/var/lib/openport ghcr.io/armanruzgar/openport
```

Then open the link it prints, `http://localhost:8080/#token=…`. Inside the container the
server listens on every interface, so it takes orders only with a write token; it keeps
one in the volume, and the link saves it in that browser tab. The `openport` volume also
keeps your accounts and chart history between runs. When nothing is trading, the
terminal offers the demo market, which needs no data at all. To use your own provider,
pass its key and arguments:

```bash
docker run --rm -p 127.0.0.1:8080:8080 -v openport:/var/lib/openport -e DATABENTO_API_KEY ghcr.io/armanruzgar/openport --provider databento --symbols SPX,QQQ
```

`docker build -t openport .` builds the same image from a checkout.

The commands that run once and exit, `--verify-run FILE`, `--backtest` and
`--import-day`, use the same image and get only the arguments you pass, for example
`docker run --rm -v openport:/var/lib/openport ghcr.io/armanruzgar/openport --verify-run /var/lib/openport/paper-journal.jsonl`.
Add `--paper-journal /var/lib/openport/paper-journal.jsonl` to a backtest to use the
server's playbooks, and write its `--out` report inside the volume.

## Docker Compose

Or use [docker-compose.yml](../docker-compose.yml) from this checkout:

```bash
docker compose up -d
docker compose exec openport openportd --print-url --write-token-file /var/lib/openport/write-token
```

Open the printed token link on the host; browser launching inside a container is
not supported. Compose uses the published image, the same named
`openport` volume and port 8080 on loopback only. It restarts unless stopped and
checks `/api/status` for health. Provider credentials and a provider command are
commented in the file. To update, run `docker compose pull && docker compose up -d`.
`docker compose down` keeps the volume; adding `--volumes` deletes its data.
Stop an existing Docker or Homebrew instance before using the same port or accounts.
Read [upgrade and backup](upgrade-and-backup.md) before updating; `OPENPORT_IMAGE`
can pin a release/digest and `OPENPORT_VOLUME` can select a restored volume.

## Linux packages

The release workflow builds `.deb` and `.rpm` packages for amd64 and arm64,
alongside the archives, with `SHA256SUMS`. Use assets from a release that includes
them; this change does not publish packages for existing releases. Verify the
checksum, then install the file for your machine:

```sh
sudo apt install ./openport-VERSION-linux-amd64.deb
# Or, on a compatible RPM distribution:
sudo dnf install ./openport-VERSION-linux-amd64.rpm
sudo systemctl daemon-reload
sudo systemctl enable --now openport
sudo openportd --print-url --write-token-file /var/lib/openport/write-token
```

These binaries are built on Ubuntu 24.04 and require glibc 2.39+, libstdc++ 13+,
OpenSSL 3, zlib, zstd and CA certificates. The deb targets Ubuntu 24.04 or compatible
systems with `libssl3t64`; an rpm is not a promise of compatibility with older
enterprise distributions. Use Docker or build from source when dependencies do
not match. ARM package names use `arm64` (RPM metadata uses `aarch64`).

The systemd unit listens on loopback, uses a private dynamic user and keeps state
under `/var/lib/openport`; logs are in `journalctl -u openport`. The unit passes
`--no-token-link`, so the journal, which `adm` and `systemd-journal` members can
read, shows the `--print-url` command rather than the token; get the link with the
`sudo openportd --print-url` command above. Packages never
start services or delete data in install/uninstall scripts. Stop before upgrades
or removal. Use `systemctl edit openport` for startup overrides, keeping the token
and data paths; then reload and restart. Copy the printed link to your desktop
browser rather than launching a browser as root.

## Windows

Use Docker Desktop with its WSL2/Linux-container backend. Run the Docker command
above in PowerShell and open `http://localhost:8080` with the printed token link.
Keep the named volume for persistence; do not replace it with an arbitrary Windows
bind mount. The browser stays on Windows, so use `--print-url` through `docker exec`
instead of `--open` inside the container.

Alternatively, install Ubuntu 24.04 in WSL2, then use the Linux package or source
instructions there. WSL localhost forwarding lets the Windows browser reach the
loopback listener. Keep native journals in WSL's Linux filesystem, not `/mnt/c`.
Use a foreground daemon when WSL systemd is disabled. No native Windows binary is
provided. See [Microsoft's WSL installation guide](https://learn.microsoft.com/windows/wsl/install)
and [Docker's WSL2 setup](https://docs.docker.com/desktop/features/wsl/).

## Release archives and source

Each [release](https://github.com/armanruzgar/openport/releases) has archives for Linux
(amd64 and arm64) and macOS (Apple Silicon).

From source (CMake 3.25+, a C++20 compiler, Boost 1.83+, OpenSSL 3, zlib, zstd and
Node 22+; everything else is fetched and pinned by checksum):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j3
(cd web && npm ci && npm run build)
./build/apps/openportd --open --symbols SPX,SPY,QQQ,IWM,DIA --web-root web/dist
```

To install it, `cmake --install build --component openport --prefix ~/.local` puts
`openportd` in `bin/` and the terminal in `share/openport/web`, where it finds it on
its own. The scenario library is compiled into the binary and needs no installed
files. A release archive has the same layout: unpack it and run `bin/openportd` (Linux
needs OpenSSL 3, zlib and zstd; the macOS archive, for Apple silicon, needs nothing
installed). `openportd --version` prints the version. The terminal opens with a short
welcome the first time; the footer's welcome link shows it again.

## Recording

Add `--record-dir ~/.openport/recordings` to record each session for the Replay page.
Full chains are large: see [recording](runtime.md#recording-and-replay) before
recording all day.

## Public demo and sandbox accounts

For a deployment configuration, use [deploy/demo](../deploy/demo/README.md).
It adds resource limits and an authenticating HTTPS proxy. For personal remote
or phone access, follow [Tailscale or Caddy](remote-access.md); public unauthenticated
personal instances are not supported by [SECURITY.md](../SECURITY.md).

Run `openportd --provider demo --address 0.0.0.0 --write-token-file PATH` with
`--allowed-host terminal.example.com` for your public name, or set
`OPENPORT_WRITE_TOKEN` instead of the file. Terminate HTTPS at the reverse proxy.
Keep the write token private; visitors can watch without it, and the terminal shows
a watch-only notice with an action to enter a token. Add `--sandboxes 100` to let
visitors create their own practice accounts and trade the simulated feed. Their
session tokens work only on their own account. Server settings still need the
operator's token. Without this option, visitors can only watch.

The demo feed uses only simulated prices. Without `--paper-journal`, it keeps the
main account at `~/.openport/demo/paper-journal.jsonl`, with accounts and replay
history beside it. Journals do not identify their feed: keep demo journals separate
from live ones, including when you supply a path. With Docker, use a separate volume:

```bash
docker run --rm -p 127.0.0.1:8080:8080 -v openport-demo:/var/lib/openport \
  ghcr.io/armanruzgar/openport --provider demo --sandboxes 100 \
    --allowed-origin https://terminal.example.com --allowed-host terminal.example.com
```

Put the HTTPS proxy in front of that local port. The image keeps its write token
in the volume; do not share the token link printed at startup. Arguments after the
image replace its default provider command. Its explicit journal path is in that
volume. Sandbox journals live in a separate `sandboxes` directory beside the demo
journal. They are deleted after 24 hours without authenticated requests and on
startup; `--sandbox-idle-seconds N` changes the idle time. Sandbox tokens are returned
once and saved in the browser tab's session storage, with an in-memory fallback.

On Railway, add `--client-ip-header X-Real-IP` so visitors do not share the proxy's
creation allowance. Configure both `--allowed-origin` and `--allowed-host` for the
public names. Trust that header only behind a proxy that overwrites it and prevents
direct access to the backend. By default, the server uses the connection address
and ignores forwarding headers. Creation allows 3 accounts per client and 30 total
per hour, with at most 100 at once in this example; each sandbox allows 60 order
requests per minute. Full capacity or rate limits return HTTP 429. The normal
low-disk journal refusal applies. [Runtime details](runtime.md#public-sandboxes).
