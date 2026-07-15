# Multi-stage build for Samosa
# Stage 1: Build
FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y \
    build-essential \
    libomp-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY src/ src/

RUN gcc -O3 -pthread -fopenmp src/qwen36b.c src/expert_cache.c -o qwen36b -lm

# Stage 2: Runtime
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y \
    libgomp1 \
    curl \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Setup release directory and volume mount targets
RUN mkdir -p /release/bin /model /samosa_home

# Copy compiled engine and launcher/wrapper
COPY --from=builder /build/qwen36b /release/bin/qwen36b
COPY dist/samosa /release/bin/samosa
RUN chmod +x /release/bin/samosa

# Copy static UI assets
COPY assets/app.html /release/app.html
COPY assets/samosa-chat.png /release/samosa-chat.png

# Symlink model folder and tokenizer from volume mount to release directory
RUN ln -s /model /release/model \
    && ln -s /model/tokenizer_qwen36.json /release/tokenizer_qwen36.json

# Environment variables
ENV SAMOSA_RELEASE_DIR=/release \
    SAMOSA_HOME=/samosa_home \
    SAMOSA_BIND=0.0.0.0 \
    PATH=/release/bin:$PATH

WORKDIR /release
EXPOSE 8642

ENTRYPOINT ["samosa"]
CMD ["serve"]
