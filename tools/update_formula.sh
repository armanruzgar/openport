#!/usr/bin/env bash
# Update Formula/openport.rb from a published release's SHA256SUMS.
# Usage: tools/update_formula.sh VERSION [SHA256SUMS]
# The optional local file permits an offline check; otherwise curl downloads it.
set -euo pipefail

fail() { echo "update_formula: $*" >&2; exit 1; }
if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
  echo "usage: tools/update_formula.sh VERSION [SHA256SUMS]" >&2
  exit 2
fi
version="${1#v}"
[[ "$version" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]] || fail "expected a release version such as 0.3.0"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
formula="$root/Formula/openport.rb"
asset="openport-$version-darwin-arm64.tar.gz"
release="https://github.com/armanruzgar/openport/releases/download/v$version"
temporary="$(mktemp -d)"
trap 'rm -rf "$temporary"' EXIT
if [ "$#" = 2 ]; then
  cp "$2" "$temporary/SHA256SUMS"
else
  curl --fail --silent --show-error --location --proto '=https' --proto-redir '=https' \
    "$release/SHA256SUMS" -o "$temporary/SHA256SUMS"
fi

# sha256sum/shasum use two spaces for text mode, or a space and '*' for binary.
# Match the whole filename; reject missing, malformed and duplicate entries.
sha="$(awk -v asset="$asset" '
  { sub(/\r$/, ""); name = $2; sub(/^\*/, "", name) }
  name == asset {
    count++
    if (NF != 2 || length($1) != 64 || $1 ~ /[^0-9a-fA-F]/) invalid = 1
    digest = tolower($1)
  }
  END { if (count != 1 || invalid) exit 1; print digest }
' "$temporary/SHA256SUMS")" || fail "expected one valid SHA256SUMS entry for $asset"

awk -v url="$release/$asset" -v version="$version" -v sha="$sha" '
  /^    url .*releases\/download/ { print "    url \"" url "\""; urls++; next }
  /^    url .*openport\.git/ { print "    url \"https://github.com/armanruzgar/openport.git\", tag: \"v" version "\""; tags++; next }
  /^  version / { print "  version \"" version "\""; versions++; next }
  /^    sha256 / { print "    sha256 \"" sha "\""; sums++; next }
  { print }
  END { if (urls != 1 || versions != 1 || sums != 1 || tags != 1) exit 1 }
' "$formula" > "$temporary/openport.rb" || fail "unexpected formula layout; left unchanged"
cat "$temporary/openport.rb" > "$formula"
echo "Updated Formula/openport.rb for $version ($sha)"
