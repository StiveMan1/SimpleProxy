#!/bin/bash
set -euo pipefail

echo "=== SimpleProxy3 Installer ==="

# --- Root check ---
if [[ "$EUID" -ne 0 ]]; then
    echo "ERROR: Please run this installer as root (sudo)."
    exit 1
fi

# --- Detect package manager ---
if command -v apt &>/dev/null; then
    PM="apt"
elif command -v pacman &>/dev/null; then
    PM="pacman"
elif command -v dnf &>/dev/null; then
    PM="dnf"
elif command -v yum &>/dev/null; then
    PM="yum"
else
    echo "ERROR: Unsupported Linux distribution."
    exit 1
fi

echo "Using package manager: $PM"

# --- Install dependencies ---
echo "Installing dependencies..."
case "$PM" in
  apt)
    apt update
    apt install -y cmake gcc libconfig-dev pkg-config
    ;;
  pacman)
    pacman -Sy --noconfirm cmake gcc libconfig pkgconf
    ;;
  dnf|yum)
    $PM install -y cmake gcc libconfig-devel pkgconf
    ;;
esac

echo "Dependencies installed."

# --- Build using CMake ---
BUILD_DIR="build"

echo "Creating build directory: $BUILD_DIR"
rm -rf "$BUILD_DIR"
mkdir "$BUILD_DIR"
cd "$BUILD_DIR"

echo "Configuring CMake..."
cmake -DCMAKE_BUILD_TYPE=Release ..

echo "Building..."
cmake --build . --config Release

echo "Build successful."

# --- Install binary and config ---
echo "Installing with CMake..."
cmake --install .  # installs to /usr/local by default

# --- Install config directory ---
if [[ ! -d /etc/simple-proxy ]]; then
    echo "Creating /etc/simple-proxy..."
    mkdir -p /etc/simple-proxy
fi

if [[ -f ../config.cfg ]]; then
    echo "Copying config.cfg..."
    cp ../config.cfg /etc/simple-proxy/config.cfg
fi

# --- Install systemd service ---
echo "Installing systemd service..."
cp ../simple-proxy.service /etc/systemd/system/simple-proxy.service

systemctl daemon-reload
systemctl enable simple-proxy
systemctl restart simple-proxy

echo "=== Installation Complete ==="
echo "SimpleProxy3 is installed and running."
echo "Binary: /usr/local/bin/SimpleProxy3"
echo "Config: /etc/simple-proxy/config.cfg"
echo "Service: systemctl status simple-proxy"
