#!/bin/sh
# Build a distribution tarball for XZile.
# Run from the project root. Creates xzile-VERSION.tar.gz (and .gz.sig if gpg is used).
#
# Usage:
#   ./build-aux/release.sh           # build tarball, optionally sign
#   ./build-aux/release.sh --sign   # build and sign with default GPG key
#   ./build-aux/release.sh --no-sign # build only (default if gpg not available)

set -e

cd "$(dirname "$0")/.."
root="$(pwd)"

# Version from configure.ac
version=$(sed -n 's/^AC_INIT(\[XZile\],\[\([^]]*\)\].*/\1/p' configure.ac)
if [ -z "$version" ]; then
  echo "release.sh: could not get version from configure.ac" >&2
  exit 1
fi

sign=false
for arg in "$@"; do
  case "$arg" in
    --sign)   sign=true ;;
    --no-sign) sign=false ;;
    -h|--help) echo "Usage: $0 [--sign|--no-sign]"; exit 0 ;;
    *) echo "Usage: $0 [--sign|--no-sign]" >&2; exit 1 ;;
  esac
done

echo "Building xzile-$version..."
./bootstrap
./configure
make distcheck

tarball="$root/xzile-$version.tar.gz"
if [ ! -f "$tarball" ]; then
  echo "release.sh: expected $tarball after distcheck" >&2
  exit 1
fi

if $sign; then
  if command -v gpg >/dev/null 2>&1; then
    gpg --detach-sign --armor "$tarball"
    echo "Signed: $tarball.asc"
  else
    echo "release.sh: --sign requested but gpg not found" >&2
    exit 1
  fi
fi

echo "Done: $tarball"
