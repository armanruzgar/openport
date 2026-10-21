# Remote and phone access

Use a private network or an authenticated HTTPS proxy. OpenPort reads are public
by default, and an unconfigured loopback bind permits writes. Never forward port
8080 from your router. Keep a write token configured even when the proxy connects
over loopback. See the [security policy](../SECURITY.md).

## Private access with Tailscale

Install Tailscale on the server and phone, sign in to the same tailnet, and restrict
access to the server with your tailnet's access policy. On the server, start a
private demo (use your regular provider and data directory for personal trading):

```sh
openportd --provider demo --address 127.0.0.1 \
  --write-token-file "$HOME/.openport/demo/write-token" --require-token \
  --allowed-host your-server.your-tailnet.ts.net \
  --allowed-origin https://your-server.your-tailnet.ts.net
```

In another terminal:

```sh
tailscale serve --bg http://127.0.0.1:8080
tailscale serve status
openportd --provider demo --print-url
```

Use the HTTPS hostname shown by Serve, replacing the example hostname in the
daemon flags. On the phone, connect Tailscale, open that HTTPS URL and enter the
token in the terminal, or privately transfer the printed URL after replacing its
`http://127.0.0.1:8080` prefix with the HTTPS origin. Keep the `#token=` fragment;
do not put a token in a query string. The helper prints the daemon's local origin,
not an inferred proxy URL. Named `read`/`trade:ACCOUNT` tokens can give another
device narrower access than the legacy administrator token.

[Tailscale Serve](https://tailscale.com/docs/reference/tailscale-cli/serve) exposes
the service to the tailnet over HTTPS; enable its HTTPS support when prompted.
Use Serve, not Funnel, for private access. `tailscale serve reset` removes the
Serve configuration. Tailnet membership/access policy supplies the network
identity check; `--require-token` also protects OpenPort API reads and WebSockets.

## Caddy with authentication

For a public DNS name, use Caddy with a cookie authentication gateway such as
Authelia. Configure the gateway's cookie domain and access rule for
`terminal.example.com`, and allow only intended users. This
[forward-auth integration](https://www.authelia.com/integration/proxies/caddy/)
protects static files, APIs and WebSocket handshakes. OpenPort's own Bearer token
continues to control paper-trading actions.

```caddyfile
terminal.example.com {
  forward_auth 127.0.0.1:9091 {
    uri /api/authz/forward-auth
    header_up -Authorization
    header_up Host {upstream_hostport}
  }
  reverse_proxy 127.0.0.1:8080 {
    header_up X-Real-IP {remote_host}
  }
  header Referrer-Policy no-referrer
}
```

Start the engine on loopback with `--write-token-file PATH --allowed-host
terminal.example.com --allowed-origin https://terminal.example.com`. Add
`--require-token` for private account data; visitors then need an OpenPort read
token as well as the gateway session. Caddy must reach the configured auth
gateway; a missing gateway must fail closed. Plain `basic_auth` on every route
conflicts with the terminal's Bearer Authorization header. Do not bypass auth for
`/api` or `/ws` to work around that conflict.

Caddy handles HTTPS and WebSockets. Forward only 80/443 to it, keep the backend
unreachable directly, and keep the public Host/Origin consistent with the daemon
flags. Only set `--client-ip-header X-Real-IP` behind a proxy that overwrites it.
The snippet assumes Caddy is the internet-facing edge; placing another proxy in
front requires a deliberate trusted-proxy/client-IP configuration.

Test anonymous access to a static page, `/api/status` and `/ws`; all must be
refused or sent to sign-in. Then test sign-in, feed updates and a disposable
simulated order from the phone. No live-provider entitlement or public-hosting
validation is implied by these examples. Check provider terms before sharing
data. [The demo deployment](../deploy/demo/README.md) supplies Compose with
simulated data, sandboxes, resource limits and the same authenticated proxy.
