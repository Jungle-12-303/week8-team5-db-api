FROM mcr.microsoft.com/devcontainers/base:jammy

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        gdb \
        valgrind \
        make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

CMD ["sleep", "infinity"]
