#!/bin/sh
# Simple installer for seewo-touchd.
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

if [ ! -x "$BASE/seewo-touchd" ]; then
    echo "binary not built, running make..." >&2
    ( cd "$BASE" && make )
fi

install -d "$SBINDIR"
install -m 755 "$BASE/seewo-touchd" "$SBINDIR/seewo-touchd"

install -d "$UNITDIR"
install -m 644 "$BASE/systemd/seewo-touchd.service" "$UNITDIR/seewo-touchd.service"

install -d "$MODDIR"
install -m 644 "$BASE/systemd/seewo-touchd.modules" "$MODDIR/seewo-touchd.conf"

ln -sf "$UNITDIR/seewo-touchd.service" \
       "$UNITDIR/multi-user.target.wants/seewo-touchd.service" 2>/dev/null || true

if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload || true
    echo "run: systemctl enable --now seewo-touchd"
fi

echo "installed:"
ls -l "$SBINDIR/seewo-touchd" \
      "$UNITDIR/seewo-touchd.service" \
      "$MODDIR/seewo-touchd.conf"
