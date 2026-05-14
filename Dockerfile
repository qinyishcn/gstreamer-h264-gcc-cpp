FROM ubuntu:20.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq && apt-get install -y -qq \
    build-essential cmake pkg-config \
    gstreamer1.0-tools gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly gstreamer1.0-libav \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY . .
RUN mkdir -p build && cd build && cmake .. 2>&1 && make -j$(nproc) 2>&1
RUN echo "=== GStreamer Version ===" && gst-inspect-1.0 --version
RUN echo "=== Verify Elements ===" && \
    gst-inspect-1.0 x264enc 2>&1 | head -3 && \
    gst-inspect-1.0 avdec_h264 2>&1 | head -3
CMD ["./build/demo", "--duration", "15"]
