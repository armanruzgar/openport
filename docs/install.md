# Installation

OpenPort is one `openportd` process that serves the web terminal on port 8080.
Install it with Homebrew on Apple Silicon, with Docker anywhere, from a release
archive or from source. [Configuration](configuration.md) lists the providers and
every flag.

## Homebrew

On an Apple Silicon Mac, with Homebrew:

```bash
brew tap 38st/openport https://github.com/38st/openport
brew install openport
brew services start openport
```

Open the `http://localhost:8080/#token=…` link in
`$(brew --prefix)/var/log/openport.log`. If it has not appeared yet, append the
contents of `$(brew --prefix)/var/openport/write-token` to
`http://localhost:8080/#token=`, percent-encoded if you replaced the generated hex
token with one that has other characters. The service keeps accounts and chart history under
`$(brew --prefix)/var/openport`; logs are in `var/log/openport.log` and
`var/log/openport.error.log`. The formula installs the self-contained macOS archive.
For Intel Macs and Linux, use Docker or the source build.

## Docker

With Docker (Cboe delayed SPX, SPY, QQQ, IWM and DIA, no key needed), from the
published image for amd64 and arm64:

```bash
docker run --rm --name openport -p 127.0.0.1:8080:8080 -v openport:/var/lib/openport ghcr.io/38st/openport
```

Then open the link it prints, `http://localhost:8080/#token=…`. Inside the container the
server listens on every interface, so it takes orders only with a write token; it keeps
one in the volume, and the link saves it in that browser tab. The `openport` volume also
keeps your accounts and chart history between runs. When nothing is trading, the
terminal offers the demo market, which needs no data at all. To use your own provider,
pass its key and arguments:

```bash
docker run --rm -p 127.0.0.1:8080:8080 -v openport:/var/lib/openport -e DATABENTO_API_KEY ghcr.io/38st/openport --provider databento --symbols SPX,QQQ
```

`docker build -t openport .` builds the same image from a checkout.

The commands that run once and exit, `--verify-run FILE`, `--backtest` and
`--import-day`, use the same image and get only the arguments you pass, for example
`docker run --rm -v openport:/var/lib/openport ghcr.io/38st/openport --verify-run /var/lib/openport/paper-journal.jsonl`.
Add `--paper-journal /var/lib/openport/paper-journal.jsonl` to a backtest to use the
server's playbooks, and write its `--out` report inside the volume.

## Docker Compose

Or use [docker-compose.yml](../docker-compose.yml) from this checkout:

```bash
docker compose up -d
docker compose logs openport
```

Open the token link in the logs. Compose uses the published image, the same named
`openport` volume and port 8080 on loopback only. It restarts unless stopped and
checks `/api/status` for health. Provider credentials and a provider command are
commented in the file. To update, run `docker compose pull && docker compose up -d`.
`docker compose down` keeps the volume; adding `--volumes` deletes its data.
Stop an existing Docker or Homebrew instance before using the same port or accounts.

## Release archives and source

Each [release](https://github.com/38st/openport/releases) has archives for Linux
(amd64 and arm64) and macOS (Apple Silicon).

From source (CMake 3.25+, a C++20 compiler, Boost 1.83+, OpenSSL 3, zlib, zstd and
Node 22+; everything else is fetched and pinned by checksum):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
(cd web && npm ci && npm run build)
./build/apps/openportd --symbols SPX,SPY,QQQ,IWM,DIA --web-root web/dist
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
  ghcr.io/38st/openport --provider demo --sandboxes 100 \
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
