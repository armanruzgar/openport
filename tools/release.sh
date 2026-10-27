#!/usr/bin/env bash
# Build an openport release on this machine, without CI:
#
#   tools/release.sh [--skip-tests] [--no-docker] [--allow-dirty] [--publish]
#
# Checks and builds the web terminal and the engine, runs every test, then writes
# to dist/:
#   openport-VERSION-OS-ARCH.tar.gz   this machine's build: bin/, share/openport/web, docs
#   openport-VERSION-linux-ARCH.tar.gz  the Linux build from the Docker image
#   SHA256SUMS, RELEASE_NOTES.md
# and tags the Docker image openport:VERSION after a smoke test. VERSION comes from
# CMakeLists.txt. Nothing leaves the machine unless --publish is given: then it creates
# a draft GitHub release for vVERSION at this commit with gh, or refreshes the draft's
# files, notes and commit when one exists. GitHub creates the tag when you publish the
# draft, so a draft can wait for more commits without tagging the wrong one.
# After publishing the final archives and SHA256SUMS, run tools/update_formula.sh VERSION
# and commit Formula/openport.rb to the default branch to update the Homebrew tap.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

tests=1 docker=1 dirty=0 publish=0
for arg in "$@"; do
  case "$arg" in
    --skip-tests) tests=0 ;;
    --no-docker) docker=0 ;;
    --allow-dirty) dirty=1 ;;
    --publish) publish=1 ;;
    -h|--help) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "release: unknown option $arg" >&2; exit 2 ;;
  esac
done

say() { printf '\n== %s\n' "$*"; }
need() { command -v "$1" >/dev/null || { echo "release: $1 is required" >&2; exit 1; }; }
need cmake; need npm; need git; need tar
[ "$docker" = 1 ] && need docker
[ "$publish" = 1 ] && need gh
sha() { if command -v sha256sum >/dev/null; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }

version="$(awk '/^  VERSION [0-9.]+$/ { print $2; exit }' CMakeLists.txt)"
[ -n "$version" ] || { echo "release: no VERSION in CMakeLists.txt" >&2; exit 1; }
commit="$(git rev-parse --short HEAD)"
if [ "$dirty" = 0 ] && [ -n "$(git status --porcelain)" ]; then
  echo "release: the working tree has changes; commit them or pass --allow-dirty" >&2
  exit 1
fi
if [ "$publish" = 1 ] && git rev-parse -q --verify "refs/tags/v$version" >/dev/null; then
  echo "release: v$version is already tagged; raise VERSION in CMakeLists.txt first" >&2
  exit 1
fi
os="$(uname -s | tr '[:upper:]' '[:lower:]')"
arch="$(uname -m)"
dist="$root/dist"
rm -rf "$dist"
mkdir -p "$dist"
echo "openport $version ($commit) on $os/$arch"

say "Web terminal"
(
  cd web
  npm ci --no-audit --no-fund
  npx tsc -p tsconfig.json
  [ "$tests" = 1 ] && npx vitest run
  npx vite build
)

say "Engine ($os/$arch)"
build="$root/build-release"
# macOS has no OpenSSL or zstd of its own: its archive links them statically.
static="$([ "$os" = darwin ] && echo ON || echo OFF)"
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release -DOPENPORT_BUILD_BENCHMARKS=OFF \
  -DOPENPORT_STATIC_DEPS="$static" -DOPENPORT_BUILD_TESTS="$([ "$tests" = 1 ] && echo ON || echo OFF)" >/dev/null
cmake --build "$build" --parallel
[ "$tests" = 1 ] && ctest --test-dir "$build" --output-on-failure --parallel 8

# One archive: the binaries, the web terminal beside them, and the documentation.
package() {  # package NAME SOURCE-PREFIX
  local name="$1" prefix="$2" stage="$dist/stage/$1"
  mkdir -p "$stage"
  cp -R "$prefix/." "$stage/"
  mkdir -p "$stage/docs"
  cp README.md LICENSE "$stage/"
  cp docs/*.md "$stage/docs/"
  tar -C "$dist/stage" -czf "$dist/$name.tar.gz" "$name"
  echo "wrote dist/$name.tar.gz"
}
native="openport-$version-$os-$arch"
cmake --install "$build" --component openport --prefix "$dist/stage/prefix-native" >/dev/null
"$dist/stage/prefix-native/bin/openportd" --version
if [ "$os" = darwin ]; then
  # Anything but the system's own libraries would need Homebrew on the user's Mac.
  for binary in "$dist/stage/prefix-native/bin/"*; do
    if otool -L "$binary" | tail -n +2 | grep -v -E '^[[:space:]]+/(usr/lib|System/Library)/'; then
      echo "release: $(basename "$binary") links the libraries above, which macOS does not have" >&2
      exit 1
    fi
  done
fi
package "$native" "$dist/stage/prefix-native"

if [ "$docker" = 1 ]; then
  say "Docker image"
  docker build -t "openport:$version" .
  name="openport-release-smoke-$$"
  docker run -d --rm --name "$name" -p 127.0.0.1::8080 "openport:$version" >/dev/null
  trap 'docker stop "$name" >/dev/null 2>&1 || true' EXIT
  port="$(docker port "$name" 8080/tcp | sed -n '1s/.*://p')"
  for _ in $(seq 1 60); do curl -fsS "http://127.0.0.1:$port/api/status" >/dev/null 2>&1 && break; sleep 1; done
  curl -fsS "http://127.0.0.1:$port/api/status" >/dev/null
  page="$(curl -fsS "http://127.0.0.1:$port/")"
  grep -q '<div id="root">' <<<"$page"
  python3 tools/smoke_trade.py "http://127.0.0.1:$port" "$(docker exec "$name" cat /var/lib/openport/write-token)"
  docker stop "$name" >/dev/null
  trap - EXIT
  echo "openport:$version answers /api/status, serves the terminal and fills an order"

  say "Linux build from the image"
  image_arch="$(docker image inspect --format '{{.Architecture}}' "openport:$version")"
  linux="openport-$version-linux-$image_arch"
  container="$(docker create "openport:$version")"
  mkdir -p "$dist/stage/prefix-linux/bin" "$dist/stage/prefix-linux/share/openport"
  docker cp "$container:/usr/local/bin/openportd" "$dist/stage/prefix-linux/bin/"
  docker cp "$container:/usr/local/bin/openport-probe" "$dist/stage/prefix-linux/bin/"
  docker cp "$container:/usr/share/openport/web" "$dist/stage/prefix-linux/share/openport/"
  docker rm "$container" >/dev/null
  package "$linux" "$dist/stage/prefix-linux"
fi

rm -rf "$dist/stage"
(cd "$dist" && sha *.tar.gz > SHA256SUMS)

previous="$(git describe --tags --abbrev=0 2>/dev/null || true)"
{
  echo "# openport $version"
  echo
  echo "Built from $commit."
  echo
  echo "## Install"
  echo
  # Publishing the release pushes the image (image.yml) and attaches both Linux archives (release-assets.yml).
  echo "- Docker (amd64 and arm64): \`docker run --rm -p 127.0.0.1:8080:8080 -v openport:/var/lib/openport ghcr.io/armanruzgar/openport:$version\`, then open the link it prints, which carries its write token; or \`docker build -t openport .\` from this tag."
  echo "- Linux archives (amd64 and arm64): need OpenSSL 3, zlib and zstd (\`apt install libssl3t64 zlib1g libzstd1\` on Ubuntu 24.04). Unpack one and run \`bin/openportd\`."
  echo "- macOS archive (Apple silicon): needs nothing installed. Unpack it with \`tar -xzf\` and run \`bin/openportd\`. Unpacked in Finder, macOS may refuse to run it as from an unidentified developer; \`xattr -dr com.apple.quarantine\` on the folder allows it."
  echo
  echo "Then open http://127.0.0.1:8080. \`openportd --help\` lists every option. \`SHA256SUMS\` covers every archive."
  echo
  if [ -n "$previous" ]; then
    echo "## Changes since $previous"
    echo
    git log --no-merges --format='- %s' "$previous..HEAD"
  else
    echo "## Changes"
    echo
    echo "The first release. See the README for what it does."
  fi
} > "$dist/RELEASE_NOTES.md"

say "Done"
ls -l "$dist"
if [ "$publish" = 1 ]; then
  head="$(git rev-parse HEAD)"
  if [ "$(gh release view "v$version" --json isDraft --jq .isDraft 2>/dev/null || true)" = true ]; then
    gh release upload "v$version" --clobber "$dist"/*.tar.gz "$dist/SHA256SUMS"
    gh release edit "v$version" --target "$head" --title "openport $version" --notes-file "$dist/RELEASE_NOTES.md"
    echo "Refreshed the draft release for v$version at $commit; publish it on GitHub when it looks right."
  else
    gh release create "v$version" --draft --target "$head" --title "openport $version" --notes-file "$dist/RELEASE_NOTES.md" \
      "$dist"/*.tar.gz "$dist/SHA256SUMS"
    echo "Created a draft release for v$version at $commit; publish it on GitHub when it looks right."
  fi
else
  echo "To publish: tools/release.sh --publish (creates or refreshes a draft GitHub release for v$version)."
fi
