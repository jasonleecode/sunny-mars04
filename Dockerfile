FROM debian:bookworm-slim
ARG MARS04_BUILD_GUI=OFF
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ cmake make pkg-config libusb-1.0-0-dev libpng-dev \
    && if [ "$MARS04_BUILD_GUI" = ON ]; then apt-get install -y --no-install-recommends libsdl2-dev libcairo2-dev; fi \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMARS04_BUILD_GUI=${MARS04_BUILD_GUI} \
    && cmake --build build -j2 \
    && ctest --test-dir build --output-on-failure
CMD ["./build/mars04-capture", "--help"]
