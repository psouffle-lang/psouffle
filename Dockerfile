FROM python:3.10-slim-bookworm@sha256:f8aa74bffbe59d02f7442dba43c9ea72b2c34cc29aa7a7edc74b29521b23bb43 AS base

ARG BUILD_JOBS=2
ENV DEBIAN_FRONTEND=noninteractive PYTHONUNBUFFERED=1 PIP_NO_CACHE_DIR=1
RUN apt-get update && apt-get install -y --no-install-recommends \
      autoconf automake bison build-essential ca-certificates cmake curl flex \
      git graphviz libreadline-dev libtool mcpp pkg-config time zlib1g-dev \
 && rm -rf /var/lib/apt/lists/*

ARG CUDD_COMMIT=fc30c18f8db2e7d03d13241038b61eb0056a734e
RUN git clone https://github.com/cuddorg/cudd.git /opt/cudd \
 && cd /opt/cudd && git checkout ${CUDD_COMMIT} \
 && autoreconf -fi && ./configure --enable-shared --prefix=/usr/local \
      CFLAGS="-O3 -fPIC" CXXFLAGS="-O3 -fPIC" \
 && make -j${BUILD_JOBS} && make install && ldconfig

FROM base AS evaluation-sources
ARG BUILD_JOBS=2
COPY docker/requirements.txt /tmp/requirements.txt
ENV CFLAGS="-O2 -fPIC -Wno-incompatible-pointer-types -Wno-error=incompatible-pointer-types"
RUN pip install pip==25.0.1 setuptools==75.8.0 wheel==0.45.1 \
 && pip install --no-build-isolation -r /tmp/requirements.txt

ARG PROBLOG_COMMIT=82652c432894dc4cdbfc4de95ddc42716003c336
RUN git clone https://github.com/Hughshine/problog.git /opt/problog \
 && cd /opt/problog && git checkout ${PROBLOG_COMMIT} \
 && pip install --no-build-isolation /opt/problog

ARG VPROBLOG_COMMIT=ecf5d8b6d2c7576b2e54ad2f59bd0beb8887a70a
COPY docker/clone_pinned.py /tmp/pinned-git/git
RUN chmod +x /tmp/pinned-git/git && export PATH=/tmp/pinned-git:$PATH \
 && git clone https://github.com/jjjxia/Vproblog.git /opt/vproblog \
 && cd /opt/vproblog && git checkout ${VPROBLOG_COMMIT} \
 && cd src/vlog-beta-sdd/sdd-package-2.0/libsdd-2.0 \
 && sed -i 's|print "\(.*\)"|print("\1")|' SConstruct \
 && sed -i "s|CCFLAGS='-std=c99 -Wall '|CCFLAGS='-std=c99 -Wall -fPIC '|" SConstruct \
 && scons -j${BUILD_JOBS} \
 && mkdir -p ../sdd-2.0/lib/Linux \
 && cp build/libsdd.a ../sdd-2.0/lib/Linux/libsdd.a \
 && cmake -S /opt/vproblog/src/vlog-beta-sdd \
      -B /opt/vproblog/src/vlog-beta-sdd/build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS="-include cstdint" -DCMAKE_C_FLAGS="-include stdint.h" \
 && cmake --build /opt/vproblog/src/vlog-beta-sdd/build --parallel ${BUILD_JOBS}

ARG RUST_TOOLCHAIN=nightly-2025-09-01
ARG SCALLOP_COMMIT=668bfb6d45ce302fd4ffa7f29916baf3c7ce36ef
ENV RUSTUP_HOME=/opt/rust/rustup CARGO_HOME=/opt/rust/cargo \
    PATH=/opt/rust/cargo/bin:$PATH
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs -o /tmp/rustup.sh \
 && sh /tmp/rustup.sh -y --default-toolchain ${RUST_TOOLCHAIN} --profile minimal \
 && git clone https://github.com/scallop-lang/scallop.git /opt/scallop \
 && cd /opt/scallop && git checkout ${SCALLOP_COMMIT}
COPY evaluation/full/scallop/scallop-fmcad.patch /tmp/scallop-fmcad.patch
RUN cd /opt/scallop && git apply /tmp/scallop-fmcad.patch

FROM evaluation-sources AS evaluation-deps
ARG BUILD_JOBS=2
COPY docker/scallop.Cargo.lock /opt/scallop/Cargo.lock
RUN cd /opt/scallop && cargo build --release --locked -p scli -j${BUILD_JOBS} \
 && install -m 0755 target/release/scli /usr/local/bin/scli

FROM base AS compiler
ARG BUILD_JOBS=2
WORKDIR /opt/psouffle
COPY CMakeLists.txt ./
COPY cmake cmake/
COPY src src/
COPY tests/regression tests/regression/
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build --parallel ${BUILD_JOBS} && cmake --install build
COPY . .
ENV SOUFFLE_BIN=/usr/local/bin/souffle
RUN python3 docker/smoke.py --compiler
CMD ["bash"]

FROM evaluation-deps AS evaluation
WORKDIR /opt/psouffle
COPY . .
COPY --from=compiler /usr/local/bin/souffle /usr/local/bin/souffle
COPY --from=compiler /usr/local/bin/souffle-compile.py /usr/local/bin/souffle-compile.py
COPY --from=compiler /usr/local/include/souffle /usr/local/include/souffle
# souffle-compile.py links this archive at its configured build-tree path.
COPY --from=compiler /opt/psouffle/build/src/libcompiled.a /opt/psouffle/build/src/libcompiled.a
ENV SOUFFLE_BIN=/usr/local/bin/souffle \
    VLOG_BIN=/opt/vproblog/src/vlog-beta-sdd/build/vlog \
    SCLI_BIN=/usr/local/bin/scli \
    LD_LIBRARY_PATH=/opt/vproblog/src/vlog-beta-sdd/build:/usr/local/lib
RUN python3 docker/smoke.py --compiler --engines
CMD ["bash"]
