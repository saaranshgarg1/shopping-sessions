FROM --platform=linux/amd64 docker.io/library/debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY src/ ./src/
RUN g++ -O3 -flto -mtune=cascadelake -fno-semantic-interposition -DNDEBUG -std=c++20 -pthread -static-libstdc++ -static-libgcc -Wall -Wextra -Wno-missing-field-initializers -Isrc src/main.cpp -o /shopping-sessions && strip /shopping-sessions

FROM --platform=linux/amd64 docker.io/library/debian:bookworm-slim
COPY --from=build /shopping-sessions /usr/local/bin/shopping-sessions
ENV DATA_DIR=/data OUTPUT_DIR=/output SCRATCH_DIR=/scratch NCPU=8 MEM_GB=24 HOME=/scratch
USER 1000:1000
ENTRYPOINT ["/usr/local/bin/shopping-sessions"]
