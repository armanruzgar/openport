# Hosted practice terminal

This deployment uses **simulated prices only** and up to 20 disposable visitor
accounts. It is ready for an operator to configure; no demo is hosted by this
repository. It never needs market-data or broker credentials.

Prerequisites: Docker Compose, a host with at least 3 GB of available RAM, a DNS
name pointing to it, inbound ports 80/443, and an existing cookie authentication
gateway. The Caddyfile uses [Authelia's forward-auth endpoint](https://www.authelia.com/integration/proxies/caddy/).
Configure that gateway's cookie domain and access rule for the demo hostname.
Authentication is required for all pages, API calls and WebSocket handshakes;
an unavailable gateway fails closed. Plain Basic Auth conflicts with OpenPort's
Bearer header and is not a substitute for this cookie flow.

```sh
cd deploy/demo
cp .env.example .env
# Fill DEMO_HOST and AUTH_UPSTREAM, choose SANDBOXES, and pin OPENPORT_IMAGE.
docker compose config --quiet
docker compose run --rm --no-deps caddy caddy validate --config /etc/caddy/Caddyfile
docker compose up -d
```

Caddy obtains HTTPS certificates. Only Caddy publishes ports; the engine's
internal network has no internet route. Caddy overwrites `X-Real-IP`, so clients
cannot forge sandbox creation buckets. Built-in limits allow 3 creations per IP
per hour, 30 globally per hour, and 60 order requests per sandbox per minute;
capacity and limit failures return HTTP 429. These do not throttle every read or
WebSocket message: restrict gateway membership and add edge protection before
inviting a large audience. CPU, RAM, PIDs, temporary storage and container logs
are bounded in Compose. Size the host and data volume for the chosen capacity;
the volume itself is not quota-managed by Compose.

Visitors sign in through the gateway, then choose **Create sandbox** in the
terminal. Sandboxes expire after an hour idle and are removed on engine restart.
The operator token remains in the private volume; never share its startup link.
Do not reuse a personal trading volume. Backups and removal follow
[upgrade and backup](../../docs/upgrade-and-backup.md); `docker compose down`
preserves volumes, and `docker compose down --volumes` deletes this demo's data
and certificate state.

Before sharing, verify an anonymous request is refused or redirected to login,
that a signed-in visitor can open a WebSocket and trade only their sandbox, and
that spoofing `X-Real-IP` does not bypass limits. Real DNS, certificates, gateway
sign-in and internet load need validation on the deployment host. No publishing
or live hosting is part of the local build.

After deploying and checking it, set `data-demo-url` on `#hosted-demo` in
`site/index.html` to its HTTPS URL. The site leaves the button hidden while the
value is empty or a placeholder.
