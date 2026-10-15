FROM ubuntu:22.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ cmake ninja-build curl ca-certificates pkg-config libsodium-dev libpng-dev libssl-dev libsasl2-dev \
    && rm -rf /var/lib/apt/lists/*
COPY scripts/install-mongo-drivers.sh /tmp/install-mongo-drivers.sh
RUN bash /tmp/install-mongo-drivers.sh /opt/mongo
ENV PKG_CONFIG_PATH=/opt/mongo/lib/pkgconfig
ENV LD_LIBRARY_PATH=/opt/mongo/lib
WORKDIR /source
COPY . .
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel 2 && ctest --test-dir build --output-on-failure

FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libsodium23 libpng16-16 libssl3 libsasl2-2 libsnappy1v5 libzstd1 zlib1g \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --uid 10001 --create-home app \
    && mkdir -p /var/lib/cpp-http/images && chown -R 10001:10001 /var/lib/cpp-http
COPY --from=build /opt/mongo/lib /opt/mongo/lib
COPY --from=build /source/build/myserver /app/myserver
COPY UI /app/UI
COPY start.sh /app/start.sh
RUN chmod 0555 /app/start.sh
ENV LD_LIBRARY_PATH=/opt/mongo/lib
ENV APP_ROOT=/app
ENV STORAGE_ROOT=/var/lib/cpp-http/images
ENV BIND_ADDRESS=0.0.0.0
WORKDIR /app
USER 10001:10001
EXPOSE 8080
ENTRYPOINT ["/app/start.sh"]
