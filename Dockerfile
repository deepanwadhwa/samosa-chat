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

# Where `samosa pull` fetches the 24 GB model from.
#
# dist/samosa ships the literal token REPO_ID_PLACEHOLDER as its default; that
# token is only substituted by tools/package_hf.py at Hugging Face packaging
# time, and this image COPYs dist/samosa directly. Without this, `samosa pull`
# resolves to .../REPO_ID_PLACEHOLDER/... and 404s on the very first command.
# Overridable at build time: --build-arg SAMOSA_REPO_ID=owner/repo
ARG SAMOSA_REPO_ID=deepanwa/Samosa-Chat-Qwen3.6-35B-A3B-group32

# Environment variables
ENV SAMOSA_RELEASE_DIR=/release \
    SAMOSA_HOME=/samosa_home \
    SAMOSA_BIND=0.0.0.0 \
    SAMOSA_BASE_URL=https://huggingface.co/${SAMOSA_REPO_ID}/resolve/main \
    PATH=/release/bin:$PATH

WORKDIR /release
EXPOSE 8642

ENTRYPOINT ["samosa"]
CMD ["serve"]
