# Contributing

Bug reports, fixes and features are welcome. For anything larger than a fix, open an
issue first to agree on the approach.

Use [GitHub Discussions](https://github.com/armanruzgar/openport/discussions) for setup
questions, practice workflows and early ideas. Bug and feature issue forms are in
`.github/ISSUE_TEMPLATE`; Status → Copy diagnostics can prefill the bug form without
sharing credentials or account data. Discussion forms cover the `Q&A` and `Ideas`
categories; a repository administrator must enable Discussions and those category
slugs (`q-a`, `ideas`) for the forms to appear.

## Building and testing

You need CMake 3.25+, a C++20 compiler (CI uses GCC 13 and Apple Clang), Boost 1.83+, OpenSSL 3,
zlib, zstd and Node 22+. Other dependencies are fetched and pinned by checksum.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DOPENPORT_WERROR=ON
cmake --build build -j
(cd build && ctest -j 8)                 # C++ tests
./build/bench/openport_bench             # pricing benchmarks
./build/bench/openport_bench_trading --benchmark_filter=BM_Margin  # pairing / joint allocation, 14 and 80 legs
cd web && npm ci && npx tsc -p tsconfig.json && npx vitest run && npm run build
```

`cd web && npm run dev` serves the terminal on :5173 against an `openportd` on :8080.

CI runs on every push and pull request: the C++ suite with GCC 13 on Ubuntu 24.04 and
Apple Clang on macOS, both with `-DOPENPORT_WERROR=ON`, again under AddressSanitizer
and UndefinedBehaviorSanitizer with GCC 13, the web checks, the Python client and a
Docker smoke test. The sanitizer job uses four parallel runners, each running
every fourth discovered CTest test with four local processes, using checked-in
CTest cost hints to start expensive tests first. Under `OPENPORT_SANITIZE`, the
slow generated-feed, multi-session replay and backtest fixtures sample intraday
snapshots while retaining openings, event edges and session boundaries; legacy
recording checks sample fewer days and the seeded margin sweep uses fewer trials.
No tests are excluded, and the regular GCC, macOS and local builds retain the
full-size workloads. Its `RelWithDebInfo` flags are `-O1 -g1 -DNDEBUG`
to reduce compile time while retaining function names and source lines in sanitizer
backtraces. To reproduce a shard after configuring with the workflow's flags, run
`ctest --test-dir build -j 4 --output-on-failure --no-tests=error -I 1,,4`
(replace `1` with `2`, `3` or `4`); omit `-I` to run the whole suite.
All four sanitizer runners restore a shared, zstd-compressed ccache capped at 1.5 GB;
only shard 1 saves it for later runs, avoiding competing uploads. Warm runs are
expected well under 20 minutes, with a 35-minute timeout for cold-cache headroom.
The macOS build is the release archive's: `-DOPENPORT_STATIC_DEPS=ON`
links OpenSSL and zstd statically, and CI checks it needs only macOS's own libraries.
Changes only to `site/`, `docs/` (other than `docs/openapi.yaml`, which the tests
read) or top-level Markdown files skip CI.

## Python and API contract

The Python package uses Python 3.10+. Its tests need pytest, jsonschema, pyyaml,
pandas, websockets and MCP 2.x. With the prepared development environment:

```sh
.venv-py/bin/python -m pytest python/tests
.venv-py/bin/python tools/contract_test.py http://127.0.0.1:8080 --token "$OPENPORT_WRITE_TOKEN" --spec docs/openapi.yaml
```

Python tests run a threaded synthetic HTTP server, with local socket pairs when
TCP binding is denied. A built C++ test binary also supplies real handler responses
for schema validation without a listener. The web suite compares core TypeScript
wire types with `docs/openapi.yaml`; JSON syntax in that YAML file avoids another
npm dependency.

The contract command checks every API GET, CSV and replay mirror, previews and
places a far-from-market limit, then cancels it in an isolated simulated replay.
It refuses an existing replay and requires durable journals to check archived
routes. It leaves the completed run in history. Use a local test instance and a
token with `read,replay` or `admin`. Docker CI runs it after the smoke test, with
`pip install jsonschema pyyaml`. Static assets and WebSocket transport have their
own C++ and web tests.

For a demo-day soak against a dedicated local server (it replaces that server's
replay), run `python3 tools/demo_soak.py http://127.0.0.1:9240 300 practice --seed 42
--json soak.json`. `--json` or `--json -` writes only JSON to stdout and progress
to stderr; omitting it keeps the text report. The sorted object includes every
summary counter, refusals by code, URL, speed, plan, actual server seed as a decimal
string, replay run id, scenario, expiry, condor leg symbols, start/end market times
and exit status (1 if a probe filled, otherwise 0). Seeds repeat the market; the
run id ties the summary to the saved replay; probe timing still depends on playback
and HTTP scheduling. Offline tests: `python3 tools/demo_soak_test.py`.

## Pull requests

- Keep each change to one purpose, with tests for the behaviour it adds or fixes.
  The simulator is a deterministic reducer: test it through `TradingSession` with
  explicit market times rather than the wall clock.
- Update the documentation in `docs/` and the README when behaviour, an API field or
  a rule changes.
- Match the surrounding code: its naming, comment density and idioms.
- Commit messages start with the area (`Settlement:`, `Terminal:`, `Cboe:`), then say
  what changed and why.

## Data

To add a market-data feed, see [writing a provider adapter](docs/providers.md) for
the event contract, offline tests and pull-request checklist.

Never commit market data. Recordings of a provider's feed, including Cboe's delayed
quotes, are covered by that provider's terms and may not be redistributed. Tests use
generated or hand-written quotes, and the demo market generates its prices.

## Landing page

The GitHub Pages site is plain HTML and CSS in `site/`, with no build step. The
Pages workflow copies `docs/screenshots/` into its artifact; do not commit another
copy of those images. Keep claims and measurements consistent with the README and
[how the numbers are made](docs/methods.md).
Check phone widths, both colour schemes and reduced motion after changes.

In the repository's Pages settings, select GitHub Actions as the source. Pushes
to `main` affecting the site, screenshots or `.github/workflows/pages.yml` then
deploy it, and the workflow can also be run by hand; until Pages is enabled the
workflow skips the deployment with a notice. Link previews use
`site/social-preview.png` (1280×640).

## Releasing

Publishing a GitHub release builds the image for amd64 and arm64 and pushes it to
`ghcr.io/armanruzgar/openport`, and attaches Linux archives plus `.deb`/`.rpm` packages
for amd64 and arm64. The packages include the web terminal and a private systemd
service, which is not started automatically. The release-assets workflow builds
them with CPack, inspects both formats and runs the extracted native binary before
upload. Checksums cover archives and packages. Ubuntu 24.04's glibc baseline
applies to the RPM too; do not advertise older enterprise distro compatibility.
On a matching Linux machine, `python3 tools/package_test.py PACKAGE_DIRECTORY
--arch amd64` (or `arm64`) checks locally built packages without installing them.
`tools/release.sh` builds a release on your own machine: it checks and tests the web
terminal and the engine, packages this machine's build and a Linux build from the Docker
image with checksums and release notes into `dist/`, and smoke-tests the image. Nothing
is published unless you pass `--publish`, which creates a draft GitHub release for the
version in `CMakeLists.txt` at the current commit, or refreshes its files when the draft
exists; GitHub tags the commit when you publish the draft.

After publishing the release and its `SHA256SUMS`, update the Homebrew formula:

```bash
tools/update_formula.sh VERSION
```

The script reads the checksum for `openport-VERSION-darwin-arm64.tar.gz` and updates
the formula's Apple Silicon URL, version, SHA-256 and Intel source tag together.
Intel Macs build the tag using Homebrew dependencies. Review and commit
`Formula/openport.rb` to the default branch so the tap receives it. Do this after
the final archives and checksums are published. For an offline check, pass a saved
file as the second argument; `python3 tools/update_formula_test.py` tests parsing
with fixture checksums. Users update with `brew update && brew upgrade openport`,
then `brew services restart openport`.

The example in `deploy/demo/` is configuration only, with simulated prices,
bounded sandboxes and an authenticating HTTPS proxy. Test real DNS, certificates
and sign-in on the target host before setting the site's hidden `data-demo-url`.
No hosting or publishing is implied by a local build.
Run `python3 tools/demo_proxy_test.py` to check its Caddy configuration against
synthetic local auth/backend containers. It requires Docker and the Caddy/Python
images named in the test; no real credentials or public listener are used.

## Licence

Contributions are accepted under the [MIT licence](LICENSE).
