# Security

## Reporting a vulnerability

Please report security problems privately, through GitHub's
[private vulnerability reporting](https://github.com/armanruzgar/openport/security/advisories/new)
(the repository's **Security** tab, then **Report a vulnerability**), not in a public
issue. Include what you ran, what happened and what an attacker could gain. Confirmed
problems are fixed on `main` and disclosed in a GitHub security advisory.

## Supported versions

Fixes land on `main` and in the next release. Only the latest release is supported.

## What counts

openport is a paper-trading simulator: it never routes an order to a broker or an
exchange. The things worth protecting are the machine it runs on, the paper accounts'
journals and any market-data API keys it is given. For example:

- a way to write (place orders, change accounts, start replays) without the write
  token on a non-loopback bind, or from another origin through the browser
- reading files outside the web root, or anything outside the recordings directory
  through the replay API
- script injection in the web terminal
- a change to a journal that recovery does not detect
- a crash or unbounded memory growth from a request or a provider response
- an API key leaking into logs, responses or recordings

By design, reads need no credentials, and writes are open to anyone who can reach a
loopback bind (see [Security](docs/configuration.md#security) in the configuration notes). Running it on a
public address without `--write-token`, HTTPS and an authenticating proxy is not a
supported setup.

Follow the [remote and phone access recipes](docs/remote-access.md) for private
Tailscale access or Caddy with cookie authentication. Authentication must cover
API reads and WebSocket handshakes as well as the terminal page. Configure a write
token even behind a loopback proxy; only trust client-IP headers that the proxy
overwrites, with no direct route to the backend. The [demo deployment](deploy/demo/README.md)
uses the same boundary and simulated data without provider credentials.

Status → Copy diagnostics exports build/platform details and error categories,
excluding raw messages, tokens, URLs, local paths and accounts. Reports stay local
until the user opens the prefilled GitHub link; submission is a separate action.
Fatal signal logs stay on disk with
owner-only permissions and contain no stack or memory dump. There is no telemetry.
Ordinary startup logs can contain a token link: do not paste whole logs into
public reports. Backups can contain credentials and should be protected like
the running data directory.
