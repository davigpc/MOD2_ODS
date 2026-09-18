# ===============================================================
# S4 Media Evidence — multi-stage build (amd64 e arm64)
# ===============================================================

# ---------- Estágio 1: builder (toolchain + testes) ----------
FROM debian:bookworm-slim AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ \
        cmake \
        make \
        libsqlite3-dev \
        libssl-dev \
        uuid-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
COPY tests ./tests
COPY third_party ./third_party

# Release build; falha de teste aqui impede a imagem final
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build -j"$(nproc)" \
 && ctest --test-dir build --output-on-failure

# ---------- Estágio 2: runtime (somente libs e binário) ------
FROM debian:bookworm-slim AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        libsqlite3-0 \
        libssl3 \
        libuuid1 \
        libstdc++6 \
        curl \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --create-home --uid 1000 s4 \
    && mkdir -p /data/media \
    && chown -R s4:s4 /data

WORKDIR /app

COPY --from=builder /src/build/s4_media_evidence /app/s4_media_evidence

USER s4

EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD curl -fsS http://127.0.0.1:8080/healthz || exit 1

VOLUME ["/data"]

CMD ["/app/s4_media_evidence", "--port", "8080", "--db", "/data/s4.db", "--media-dir", "/data/media"]