# SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
# SPDX-License-Identifier: GPL-3.0-or-later

FROM debian:bookworm-slim AS builder
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates cmake g++ git make ninja-build pkg-config python3 patchelf \
    libsndfile1-dev libpng-dev libsqlite3-dev libtiff-dev libfreetype6-dev \
    libssl-dev zlib1g-dev libusb-1.0-0-dev libx11-dev libwebp-dev \
    libasound2-dev libpulse-dev libxcursor-dev libxi-dev libxrandr-dev \
    libxfixes-dev libxss-dev libxkbcommon-dev libwayland-dev libegl1-mesa-dev \
    libgl1-mesa-dev libudev-dev libdbus-1-dev libdecor-0-dev \
    && rm -rf /var/lib/apt/lists/*
RUN git clone --depth 1 --branch release-3.2.28 https://github.com/libsdl-org/SDL.git /sdl \
    && cmake -S /sdl -B /sdl-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
       -DCMAKE_INSTALL_PREFIX=/opt/sdl -DSDL_SHARED=OFF -DSDL_STATIC=ON \
       -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
    && cmake --build /sdl-build --target install --parallel 2
WORKDIR /source
COPY . .
ARG BUILD_JOBS=2
ARG SMOKE_TESTS=OFF
RUN --mount=type=cache,id=ekphore-debian12-build,target=/build \
    set -- -U CMAKE_PROJECT_ekphore_INCLUDE \
    && if [ "$SMOKE_TESTS" = ON ]; then \
      set -- "$@" -DCMAKE_PROJECT_ekphore_INCLUDE=/source/agents/ekphore/docker/Smoke.cmake; \
      smoke_target=ekphore-linux-smoke; \
    else smoke_target=; fi \
    && mkdir /checks \
    && cmake -S ekphore -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH=/opt/sdl \
    -DEKPHORE_BUILD_CLAP=ON -DEKPHORE_BUILD_LV2=ON -DEKPHORE_BUILD_VST3=ON \
    -DEKPHORE_BUILD_AU=OFF -DEKPHORE_BUILD_STANDALONE_UI=ON \
    -DLIBNKS_BUILD_EXAMPLES=OFF -DLIBNKI_BUNDLED_KEY_PATH= \
    -DEKPHORE_BUNDLE_UNSUPPORTED_KONTAKT_FONTS=OFF \
    "$@" \
    && cmake --build /build --parallel ${BUILD_JOBS} \
       --target ekphore-ui ekphore-clap ekphore-lv2 ekphore-vst3 nksd $smoke_target \
    && python3 ekphore/cmake/ci/stage.py /build /artifacts \
       --standalone --formats clap lv2 vst3 \
    && mkdir -p /artifacts/licenses/sdl \
    && cp /sdl/LICENSE.txt /artifacts/licenses/sdl/LICENSE.txt \
    && python3 ekphore/cmake/ci/bundle.py /artifacts \
    && if [ "$SMOKE_TESTS" = ON ]; then \
         cp /build/ekphore-linux-smoke /checks/; \
         python3 ekphore/cmake/ci/bundle.py /checks; \
       fi

FROM debian:bookworm-slim AS runtime
COPY --from=builder /artifacts /artifacts
COPY --from=builder /checks /checks
COPY --from=builder /source/ekphore/cmake/ci/linux-check.sh /usr/local/bin/ekphore-linux-check
ENV LIBNKS_RUNTIME_DIR=/tmp/ekphore-nks-smoke
RUN mv /artifacts '/opt/ekphore relocated' && sh /usr/local/bin/ekphore-linux-check
ENTRYPOINT ["/bin/sh", "/usr/local/bin/ekphore-linux-check"]

FROM scratch AS artifacts
COPY --from=runtime ["/opt/ekphore relocated/", "/"]
