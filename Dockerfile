FROM --platform=linux/amd64 gcc:13-bookworm AS build
WORKDIR /build
COPY src/ ./src/
RUN g++ -O3 -DNDEBUG -std=c++20 -pthread -static-libstdc++ -static-libgcc -Wall -Wextra -Wno-missing-field-initializers -Isrc src/main.cpp -o /shopping-sessions

FROM --platform=linux/amd64 debian:bookworm-slim
COPY --from=build /shopping-sessions /usr/local/bin/shopping-sessions
ENV DATA_DIR=/data OUTPUT_DIR=/output SCRATCH_DIR=/scratch NCPU=8 MEM_GB=24 HOME=/scratch
USER 1000:1000
ENTRYPOINT ["/usr/local/bin/shopping-sessions"]
