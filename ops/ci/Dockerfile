FROM gcc:16.2
RUN apt-get update \
 && apt-get install -y --no-install-recommends ninja-build libssl-dev perl python3 python3-pip curl ca-certificates git \
 && pip install --break-system-packages cmake \
 && rm -rf /var/lib/apt/lists/*
