#!/usr/bin/env bash
set -euo pipefail

SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../components/device_crypto/third_party/monocypher" && pwd)"

cd "$SOURCE_DIR"
shasum -a 256 -c - <<'CHECKSUMS'
5f8360e4c06ddcc584bdb4b210c6af824c4bb301e6a9a521869b6d90795ca4b3  LICENCE.md
57eb914fc88136119bd41655cccb8c250048bf54d470540625186f8ab16f64be  monocypher.c
c494da712122da7ff679fdcf318a5317e84972b6c950fe9d896212947797facd  monocypher.h
60fce3578fb00b00da96490653d993c4cb427b1e1be38183285c66e04d22cc18  monocypher-ed25519.c
abc4fad381879f5c29176ebe014b9189956b3dfe0a3e36459b6990bc57212380  monocypher-ed25519.h
CHECKSUMS
