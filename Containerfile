# WASM toolchain — reproduces the `deploy` job's Emscripten build locally.
#
# Why this exists: `make wasm` is otherwise unbuildable on a dev machine
# without emsdk, and the deploy job is the ONLY CI job that builds WASM —
# it does not run on pull requests. Without this, a change to WASM_EXPORTS
# is unverified until after merge, at which point a push to main also
# republishes the rolling `latest` release that downstream consumers pin.
#
# Pinned to the same emsdk version .github/workflows/deploy.yml uses; bump
# both together. That image is amd64-only, so on Apple silicon this runs
# emulated — see the --platform flag in the Makefile's container targets.
#
# Note this image's base is Ubuntu 22.04 (GCC 11), which is OLDER and
# stricter than CI's ubuntu-latest. For a native build matching CI, use
# Containerfile.linux instead.
ARG EMSDK_VERSION=3.1.64
FROM emscripten/emsdk:${EMSDK_VERSION}

RUN apt-get update && \
    apt-get install -y --no-install-recommends make \
    && apt-get clean && rm -rf /var/lib/apt/lists/*

WORKDIR /work

CMD ["bash"]
