#!/bin/sh
# Simple installer for nbm-touchd.
# Run as root from the repository root, or use `make install` instead.
set -e

BASE="$(cd "$(dirname "$0")/.." && pwd)"
PREFIX="${PREFIX:-/usr/local}"
SBINDIR="$PREFIX/sbin"
UNITDIR="/etc/systemd/system"
MODDIR="/etc/modules-load.d"

if [ "$(id -u)" -ne 0 ]; then
    echo "must be run as root" >&2
    exit 1
fi

if [ ! -x "$BASE/nbm-touchd" ]; then
    echo "binary not built, running make..." >&2
    ( cd "$BASE" && make )
fi

install -d "$SBINDIR"
install -m 755 "$BASE/nbm-touchd" "$SBINDIR/nbm-touchd"

install -d "$UNITDIR"
install -m 644 "$BASE/systemd/nbm-touchd.service" "$UNITDIR/nbm-touchd.service"

install -d "$MODDIR"
install -m 644 "$BASE/systemd/nbm-touchd.modules" "$MODDIR/nbm-touchd.conf"

ln -sf "$UNITDIR/nbm-touchd.service" \
       "$UNITDIR/multi-user.target.wants/nbm-touchd.service" 2>/dev/null || true

if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload || true
    echo "run: systemctl enable --now nbm-touchd"
fi

echo "installed:"
ls -l "$SBINDIR/nbm-touchd" \
      "$UNITDIR/nbm-touchd.service" \
      "$MODDIR/nbm-touchd.conf"
