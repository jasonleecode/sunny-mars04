FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ cmake make pkg-config libusb-1.0-0-dev libpng-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j2 \
    && ctest --test-dir build --output-on-failure
CMD ["./build/mars04-capture", "--help"]
