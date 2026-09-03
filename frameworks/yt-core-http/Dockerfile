# syntax=docker/dockerfile:1.7
#
# yt/yt/core/http lives inside the ytsaurus monorepo and is only built
# through that monorepo's own CMake+Conan project (see BUILD.md in the
# ytsaurus repo) -- there is no standalone package for it. This stage
# clones the repo, drops a small extra CMake target ("httparena/") next to
# it that links against the yt-core-http library the monorepo already
# builds, and compiles just that target's dependency closure. Configure
# still has to see the whole monorepo's CMake graph (the root file is a
# flat `add_subdirectory` fan-out with no finer-grained include), but
# `ninja <target>` only ever compiles what httparena-yt-core-http actually
# depends on -- in practice yt/yt/core, its contrib (openssl, zlib, lz4,
# zstd, brotli, protobuf, abseil, ...) and nothing from yt/yt/server or
# yql, which is why the tree-wide `yql` subdirectory is skipped below
# instead of built (it needs an antlr4 install we have no other use for).
FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        curl \
        g++-14 \
        gnupg \
        git \
        libidn-dev \
        libstdc++-14-dev \
        m4 \
        ninja-build \
        python3 \
        python3-dev \
        python3-pip \
        unzip \
    && rm -rf /var/lib/apt/lists/*

# clang-20/lld-20 are what ytsaurus/clang.toolchain hardcodes as the
# compiler; nothing else in this repo's CMake export tolerates a
# different major version.
ARG LLVM_VERSION=20
RUN curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key -o /usr/share/keyrings/llvm.key \
    && gpg --dearmor -o /usr/share/keyrings/llvm-archive-keyring.gpg /usr/share/keyrings/llvm.key \
    && CODENAME="$(. /etc/os-release && echo "$VERSION_CODENAME")" \
    && echo "deb [signed-by=/usr/share/keyrings/llvm-archive-keyring.gpg] http://apt.llvm.org/${CODENAME}/ llvm-toolchain-${CODENAME}-${LLVM_VERSION} main" \
        > /etc/apt/sources.list.d/llvm.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends "clang-${LLVM_VERSION}" "lld-${LLVM_VERSION}" "llvm-${LLVM_VERSION}" \
    && rm -rf /var/lib/apt/lists/*

# cmake/llvm-tools.cmake (pulled in unconditionally by the root CMakeLists,
# not just by the yql codegen we skip) only ever looks for llvm-link/opt/
# llc/llvm-as under the -12, -14 or unversioned names, never -20.
RUN for tool in llvm-link opt llc llvm-as; do \
        ln -s "/usr/bin/${tool}-${LLVM_VERSION}" "/usr/bin/${tool}"; \
    done

RUN pip3 install --break-system-packages --no-cache-dir "conan==2.28.1" "PyYAML==6.0.1" dacite \
    && conan profile detect

ARG PROTOC_VERSION=3.20.3
RUN curl -fsSL -o /tmp/protoc.zip \
        "https://github.com/protocolbuffers/protobuf/releases/download/v${PROTOC_VERSION}/protoc-${PROTOC_VERSION}-linux-x86_64.zip" \
    && unzip -q /tmp/protoc.zip -d /usr/local \
    && rm /tmp/protoc.zip

# Pinned to a commit rather than a branch so the entry does not silently
# start (or stop) building on an unrelated upstream change.
ARG YTSAURUS_COMMIT=c4eeebcd120657da24907aa3c5ddbd8bef638de2
RUN git init /workspace/ytsaurus \
    && cd /workspace/ytsaurus \
    && git remote add origin https://github.com/ytsaurus/ytsaurus.git \
    && git fetch --depth 1 origin "${YTSAURUS_COMMIT}" \
    && git checkout FETCH_HEAD

WORKDIR /workspace/ytsaurus
COPY main.cpp CMakeLists.txt ./httparena/
RUN sed -i \
        -e 's/^add_subdirectory(yql)$/# add_subdirectory(yql)  # unrelated to yt-core-http; needs an antlr4 install we do not carry/' \
        CMakeLists.linux-x86_64.txt \
    && echo 'add_subdirectory(httparena)' >> CMakeLists.linux-x86_64.txt

RUN mkdir -p /workspace/build && cd /workspace/build \
    && cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=/workspace/ytsaurus/clang.toolchain \
        -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=/workspace/ytsaurus/cmake/conan_provider.cmake \
        /workspace/ytsaurus \
    && ninja httparena-yt-core-http

# The binary statically links Arcadia's own vendored libc++ (the toolchain
# passes -nostdinc++/-nodefaultlibs and pulls contrib/libs/cxxsupp in by
# hand), so the only runtime shared libraries it needs are glibc's own --
# `ldd` on the built binary shows just libc.so.6 and libm.so.6.
FROM ubuntu:24.04
COPY --from=build /workspace/build/httparena/httparena-yt-core-http /server
EXPOSE 8080
CMD ["/server"]
