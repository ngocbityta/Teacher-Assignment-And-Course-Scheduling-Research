# Build stage
FROM ubuntu:22.04 AS builder

# Install build dependencies (including or-tools build deps)
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    pkg-config \
    libssl-dev \
    zlib1g-dev \
    uuid-dev \
    libc-ares-dev \
    libbrotli-dev \
    libzstd-dev \
    libyaml-cpp-dev \
    libjsoncpp-dev \
    wget \
    curl \
    unzip \
    python3 \
    python3-pip \
    autoconf \
    automake \
    libtool \
    bison \
    flex \
    && rm -rf /var/lib/apt/lists/*

# Build or-tools from source using CMake with single job to minimize memory usage
# Note: Using -j1 (single job) uses much less memory but takes longer (2-3 hours)
# Alternative: Increase Docker memory limit to 4GB+ and use -j2 or -j4
WORKDIR /tmp
RUN git clone --depth 1 --branch v9.8 https://github.com/google/or-tools.git && \
    cd or-tools && \
    mkdir build && \
    cd build && \
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_DEPS=ON \
        -DBUILD_PYTHON=OFF \
        -DBUILD_JAVA=OFF \
        -DBUILD_CXX=ON \
        -DCMAKE_INSTALL_PREFIX=/opt/or-tools && \
    cmake --build . --config Release -j1 && \
    cmake --install . && \
    cd / && \
    rm -rf /tmp/or-tools

# Build Drogon from source
# Using latest stable version and proper submodule initialization
WORKDIR /tmp
RUN git clone --depth 1 https://github.com/drogonframework/drogon.git && \
    cd drogon && \
    git submodule update --init --recursive && \
    mkdir build && \
    cd build && \
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=OFF && \
    make -j2 && \
    make install && \
    cd / && \
    rm -rf /tmp/drogon

# nlohmann/json will be handled by CMake FetchContent in CMakeLists.txt

WORKDIR /app

# Copy CMakeLists.txt and source files
COPY CMakeLists.txt .
COPY src ./src
COPY config.json .
COPY testcases ./testcases

# Create build directory and build
RUN mkdir -p build && cd build && \
    cmake .. \
    -DCMAKE_PREFIX_PATH="/opt/or-tools;/usr/local" \
    -DCMAKE_BUILD_TYPE=Release && \
    cmake --build . --config Release

# Runtime stage
FROM ubuntu:22.04

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    libssl3 \
    libc-ares2 \
    libbrotli1 \
    libzstd1 \
    libyaml-cpp0.7 \
    libjsoncpp25 \
    ca-certificates \
    wget \
    && rm -rf /var/lib/apt/lists/*

# Copy Drogon runtime libraries
COPY --from=builder /usr/local/lib/libdrogon.so* /usr/local/lib/
COPY --from=builder /usr/local/lib/libtrantor.so* /usr/local/lib/

# Copy or-tools runtime
COPY --from=builder /opt/or-tools /opt/or-tools

# Set library path
ENV LD_LIBRARY_PATH=/usr/local/lib:/opt/or-tools/lib:${LD_LIBRARY_PATH}

WORKDIR /app

# Copy built binary and config
COPY --from=builder /app/build/bin/teacher_scheduler .
COPY --from=builder /app/config.json .
COPY --from=builder /app/testcases ./testcases

EXPOSE 8081

CMD ["./teacher_scheduler", "--config=config.json"]

