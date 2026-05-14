#!/bin/bash
# H.264 + GCC Video System - Ubuntu 20.04 Installation
# Supports: Ubuntu 20.04 LTS (Focal Fossa)
# GStreamer version: 1.16.x

set -e

echo "========================================="
echo "H.264 + GCC C++ - Ubuntu 20.04 Setup"
echo "========================================="

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "WARNING: Not running as root. Using sudo for package installation."
    SUDO="sudo"
else
    SUDO=""
fi

# Detect Ubuntu version
if [ -f /etc/os-release ]; then
    . /etc/os-release
    echo "Detected: $NAME $VERSION_ID"
fi

# Update package lists
echo ""
echo ">>> Updating package lists..."
$SUDO apt-get update -qq

# Install build tools
echo ""
echo ">>> Installing build tools..."
$SUDO apt-get install -y -qq \
    build-essential \
    cmake \
    pkg-config

# Install GStreamer 1.16 core + dev
echo ""
echo ">>> Installing GStreamer 1.16..."
$SUDO apt-get install -y -qq \
    gstreamer1.0-tools \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev

# Install Python GI bindings (for optional Python tools)
echo ""
echo ">>> Installing Python GI bindings..."
$SUDO apt-get install -y -qq \
    python3-gi \
    gir1.2-gstreamer-1.0 \
    gir1.2-gst-plugins-base-1.0 \
    2>/dev/null || echo "  (Python GI optional, skipping)"

# Verify GStreamer installation
echo ""
echo ">>> Verifying GStreamer installation..."
for elem in x264enc avdec_h264 rtph264pay rtph264depay rtpbin udpsrc udpsink videotestsrc videoconvert h264parse; do
    if gst-inspect-1.0 "$elem" &>/dev/null; then
        echo "  ✅ $elem"
    else
        echo "  ❌ $elem (missing)"
    fi
done

# Verify GStreamer version
echo ""
echo ">>> GStreamer version:"
gst-inspect-1.0 --version 2>/dev/null || echo "  (version check not available)"

echo ""
echo "========================================="
echo "Installation complete!"
echo "========================================="
echo ""
echo "Build the project:"
echo "  mkdir build && cd build"
echo "  cmake .."
echo "  make -j\$(nproc)"
echo ""
echo "Run demo:"
echo "  ./build/demo --duration 20"
echo ""
echo "Run tests:"
echo "  ./build/sender &"
echo "  sleep 1"
echo "  ./build/receiver"
