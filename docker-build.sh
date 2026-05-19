#!/usr/bin/env bash
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.
# The ASF licenses this file to You under the Apache License, Version 2.0
# (the "License"); you may not use this file except in compliance with
# the License.  You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# =============================================================================
# docker-build.sh — Build/run the Gluten-Velox container on AWS ARM64 EC2
# =============================================================================
#
# PRIMARY TARGET: linux/arm64 (AWS Graviton2/3, aarch64)
#
# Usage:
#   ./docker-build.sh                      # Full build, Spark 3.5, arm64
#   ./docker-build.sh --spark=3.3          # Build for Spark 3.3
#   ./docker-build.sh --spark=3.4          # Build for Spark 3.4
#   ./docker-build.sh --threads=8          # Parallel threads (default: nproc-2)
#   ./docker-build.sh --shell              # Interactive bash inside container
#   ./docker-build.sh --rebuild-image      # Force rebuild the Docker image
#   ./docker-build.sh --platform=amd64     # Override to amd64 (dev only)
#
# Workflow for AWS ARM64 EC2:
#   git clone <your-fork> && cd gluten-velox
#   ./docker-build.sh --threads=<half-your-vcpus>
#   # JARs land in ./output/
#
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---------------------------------------------------------------------------
# Detect host architecture — default platform matches the host
# On an AWS Graviton EC2 (aarch64) this auto-selects arm64.
# On an x86_64 dev machine this auto-selects amd64.
# ---------------------------------------------------------------------------
HOST_ARCH="$(uname -m)"
if [[ "${HOST_ARCH}" == "aarch64" || "${HOST_ARCH}" == "arm64" ]]; then
  DEFAULT_PLATFORM="linux/arm64"
else
  DEFAULT_PLATFORM="linux/amd64"
fi

IMAGE_NAME="gluten-velox-builder"
SPARK_VERSION="3.5"
NUM_THREADS=$(( $(nproc 2>/dev/null || echo 8) - 2 ))
NUM_THREADS=$(( NUM_THREADS < 1 ? 1 : NUM_THREADS ))
INTERACTIVE_SHELL=false
FORCE_REBUILD_IMAGE=false
PLATFORM="${DEFAULT_PLATFORM}"
OUTPUT_DIR="${SCRIPT_DIR}/output"

# ---------------------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------------------
for arg in "$@"; do
  case $arg in
    --spark=*)      SPARK_VERSION="${arg#*=}" ;;
    --threads=*)    NUM_THREADS="${arg#*=}" ;;
    --shell)        INTERACTIVE_SHELL=true ;;
    --rebuild-image) FORCE_REBUILD_IMAGE=true ;;
    --platform=amd64) PLATFORM="linux/amd64" ;;
    --platform=arm64) PLATFORM="linux/arm64" ;;
    --help|-h)
      echo "Usage: $0 [--spark=3.3|3.4|3.5] [--threads=N] [--shell] [--rebuild-image] [--platform=arm64|amd64]"
      exit 0
      ;;
    *)
      echo "Unknown argument: $arg"
      echo "Run '$0 --help' for usage."
      exit 1
      ;;
  esac
done

# Derive a tag that encodes the target arch so amd64 and arm64 images don't clash
ARCH_TAG="${PLATFORM//linux\//}"        # "arm64" or "amd64"
FULL_IMAGE="${IMAGE_NAME}:${ARCH_TAG}"

echo "============================================================"
echo " Gluten + Velox Backend — Docker Build Helper"
echo " Host arch     : ${HOST_ARCH}"
echo " Target arch   : ${ARCH_TAG}  (platform=${PLATFORM})"
echo " Image         : ${FULL_IMAGE}"
echo " Spark version : ${SPARK_VERSION}"
echo " Threads       : ${NUM_THREADS}"
echo " Interactive   : ${INTERACTIVE_SHELL}"
echo "============================================================"

# ---------------------------------------------------------------------------
# Warn if host ≠ target (requires QEMU binfmt for cross-build)
# ---------------------------------------------------------------------------
if [[ "${PLATFORM}" == "linux/arm64" && "${HOST_ARCH}" != "aarch64" && "${HOST_ARCH}" != "arm64" ]]; then
  echo ""
  echo "⚠️  WARNING: You are building an arm64 image on an x86_64 host."
  echo "   This requires Docker buildx + QEMU binfmt_misc emulation."
  echo "   Install with: docker run --privileged --rm tonistiigi/binfmt --install arm64"
  echo "   For a real build, push your code and run this on an AWS Graviton EC2 instead."
  echo ""
fi

# ---------------------------------------------------------------------------
# Build Docker image if needed
# ---------------------------------------------------------------------------
if [[ "${FORCE_REBUILD_IMAGE}" == "true" ]] || \
   ! docker image inspect "${FULL_IMAGE}" &>/dev/null; then
  echo ""
  echo ">>> Building Docker image: ${FULL_IMAGE} (platform=${PLATFORM}) ..."
  docker buildx build \
    --platform "${PLATFORM}" \
    --load \
    --build-arg SPARK_VERSION="${SPARK_VERSION}" \
    --build-arg NUM_THREADS="${NUM_THREADS}" \
    -f "${SCRIPT_DIR}/Dockerfile.velox-build" \
    -t "${FULL_IMAGE}" \
    "${SCRIPT_DIR}"
  echo ">>> Docker image built successfully: ${FULL_IMAGE}"
else
  echo ">>> Image '${FULL_IMAGE}' exists. Use --rebuild-image to force rebuild."
fi

mkdir -p "${OUTPUT_DIR}"

# ---------------------------------------------------------------------------
# Run: interactive shell OR full build
# ---------------------------------------------------------------------------
DOCKER_RUN_ARGS=(
  --rm
  --platform "${PLATFORM}"
  -e NUM_THREADS="${NUM_THREADS}"
  -e SPARK_VERSION="${SPARK_VERSION}"
  -e CPU_TARGET="aarch64"
  # Bind-mount the source so edits on the host are immediately inside
  -v "${SCRIPT_DIR}:/gluten"
  # Bind-mount output dir so JARs are accessible on the host after build
  -v "${OUTPUT_DIR}:/output"
  -w /gluten
)

if [[ "${INTERACTIVE_SHELL}" == "true" ]]; then
  echo ""
  echo ">>> Starting interactive shell inside: ${FULL_IMAGE}"
  echo "    Source mounted at : /gluten"
  echo "    Output mounted at : /output"
  echo ""
  echo "    To build manually:"
  echo "      ./dev/buildbundle-veloxbe.sh --spark_version=${SPARK_VERSION}"
  echo ""
  docker run "${DOCKER_RUN_ARGS[@]}" -it "${FULL_IMAGE}" /bin/bash
else
  echo ""
  echo ">>> Running full build for Spark ${SPARK_VERSION} on ${PLATFORM} ..."
  docker run "${DOCKER_RUN_ARGS[@]}" "${FULL_IMAGE}"

  echo ""
  echo "============================================================"
  echo " Copying built JARs to: ${OUTPUT_DIR}"
  echo "============================================================"
  find "${SCRIPT_DIR}/package/target/" -name "*.jar" \
    -not -name "*sources*" \
    -not -name "*javadoc*" \
    -exec cp -v {} "${OUTPUT_DIR}/" \; 2>/dev/null || true

  echo ""
  echo "Built artifacts:"
  ls -lh "${OUTPUT_DIR}"/*.jar 2>/dev/null || echo "(no JARs found — check build logs above)"

  echo ""
  echo "To deploy: copy the JAR to your Spark cluster's extraClassPath"
  echo "  e.g.: gluten-velox-bundle-spark3.5_2.12-*.jar"
fi
