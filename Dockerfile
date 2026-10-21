# OpenPort in one container: the C++ engine and the web terminal it serves.
#
#   docker build -t openport .
#   docker run --rm -p 8080:8080 openport                              # Cboe delayed, no key
#   docker run --rm -p 8080:8080 -e DATABENTO_API_KEY openport --provider databento --symbols SPX,SPY

FROM node:26-slim AS web
WORKDIR /web
COPY web/package.json web/package-lock.json ./
RUN npm ci --no-audit --no-fund
COPY web/ ./
# The type check includes web tests, which share the C++ template parity fixture.
COPY tests/data/template-parity.json /tests/data/template-parity.json
RUN npm run build

FROM ubuntu:24.04 AS build
ARG DEBIAN_FRONTEND=noninteractive
ARG OPENPORT_BUILD_REVISION=unknown
# Ubuntu's mirrors can list a package for a while after it stops serving it (a 404),
# so each apt step retries from a fresh index.
RUN for attempt in 1 2 3; do \
      apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build ca-certificates \
        libboost-dev libssl-dev zlib1g-dev libzstd-dev && break; \
      [ "$attempt" -lt 3 ] || exit 1; sleep 30; \
    done \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY cmake cmake
COPY include include
COPY src src
COPY apps apps
COPY scenarios scenarios
COPY tests tests
COPY bench bench
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DOPENPORT_BUILD_REVISION="$OPENPORT_BUILD_REVISION" \
      -DOPENPORT_BUILD_TESTS=OFF -DOPENPORT_BUILD_BENCHMARKS=OFF \
    && cmake --build build --target openportd openport-probe -j3

FROM ubuntu:24.04
ARG DEBIAN_FRONTEND=noninteractive
RUN for attempt in 1 2 3; do \
      apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates libssl3t64 zlib1g libzstd1 && break; \
      [ "$attempt" -lt 3 ] || exit 1; sleep 30; \
    done \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --home /var/lib/openport openport \
    && mkdir -p /var/lib/openport \
    && chown openport:openport /var/lib/openport
COPY --from=build /src/build/apps/openportd /src/build/apps/openport-probe /usr/local/bin/
COPY --from=web /web/dist /usr/share/openport/web
VOLUME ["/var/lib/openport"]
ENV HOME=/var/lib/openport
USER openport
EXPOSE 8080
# Listening on every interface, the server takes orders only with a write token: it
# keeps one in the volume and prints a link that saves it in a browser tab.
ENTRYPOINT ["openportd", "--address", "0.0.0.0", "--paper-journal", "/var/lib/openport/paper-journal.jsonl", "--write-token-file", "/var/lib/openport/write-token", "--web-root", "/usr/share/openport/web"]
CMD ["--provider", "cboe", "--symbols", "SPX,SPY,QQQ,IWM,DIA"]
