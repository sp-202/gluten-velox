#!/usr/bin/env bash
# =============================================================================
# build-native.sh — Native Gluten + Velox build for ARM64 Ubuntu 24.04
# Target: AWS r7g.4xlarge (Graviton3, 16 vCPU, 128 GB RAM)
#
# Usage:
#   ./build-native.sh              # full build, Spark 3.5, 14 threads
#   ./build-native.sh --threads=8  # override thread count
#   ./build-native.sh --spark=3.4  # different Spark version
#   ./build-native.sh --setup-only # install deps and stop
#   ./build-native.sh --skip-setup # skip apt/gflags/glog, go straight to build
#
# Run as a non-root user that has passwordless sudo, OR as root.
# Output JAR lands in ./output/
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTPUT_DIR="${SCRIPT_DIR}/output"

# ---------------------------------------------------------------------------
# Defaults — tuned for r7g.4xlarge (16 vCPU / 128 GB)
# Leave 2 cores for the OS; 14 compile threads is safe with 128 GB.
# ---------------------------------------------------------------------------
NUM_THREADS=${NUM_THREADS:-16}
SPARK_VERSION="3.5"
SETUP_ONLY=false
SKIP_SETUP=false

for arg in "$@"; do
  case $arg in
    --threads=*)  NUM_THREADS="${arg#*=}" ;;
    --spark=*)    SPARK_VERSION="${arg#*=}" ;;
    --setup-only) SETUP_ONLY=true ;;
    --skip-setup) SKIP_SETUP=true ;;
    --help|-h)
      grep '^#' "$0" | head -20 | sed 's/^# \?//'
      exit 0
      ;;
    *) echo "Unknown arg: $arg (try --help)"; exit 1 ;;
  esac
done

# ---------------------------------------------------------------------------
# Verify we are on ARM64 Ubuntu 24.04
# ---------------------------------------------------------------------------
ARCH="$(uname -m)"
if [[ "${ARCH}" != "aarch64" ]]; then
  echo "ERROR: This script targets aarch64 (ARM64). Detected: ${ARCH}"
  exit 1
fi

echo "============================================================"
echo " Gluten + Velox Native Build  —  ARM64 Ubuntu 24.04"
echo " Arch    : ${ARCH}"
echo " Spark   : ${SPARK_VERSION}"
echo " Threads : ${NUM_THREADS}"
echo " Output  : ${OUTPUT_DIR}"
echo "============================================================"

# ---------------------------------------------------------------------------
# Step 1 — System packages
#
# Ubuntu 24.04 (noble) ships GCC 13 by default — that's what we use.
# We deliberately do NOT install libgflags-dev or libgoogle-glog-dev from apt:
#   - Ubuntu 24.04 ARM64 libgflags.a is NOT compiled with -fPIC → linker error
#     when building libvelox.so (R_AARCH64_ADR_PREL_PG_HI21 relocation error)
#   - We build both from source below so we control the -fPIC flag
# ---------------------------------------------------------------------------
if [[ "${SKIP_SETUP}" == "false" ]]; then
  echo ""
  echo ">>> [1/5] Installing system packages..."
  sudo apt-get update -y
  sudo apt-get install -y --no-install-recommends \
    build-essential \
    gcc g++ \
    git curl wget unzip tar patch \
    ninja-build ccache pkg-config \
    cmake \
    libtool autoconf automake bison flex \
    python3 python3-pip python3-venv \
    openjdk-17-jdk \
    maven \
    libboost-all-dev \
    libgeos-dev \
    libssl-dev \
    libcurl4-openssl-dev \
    libdouble-conversion-dev \
    libre2-dev \
    libfmt-dev \
    liblz4-dev \
    libzstd-dev \
    libsnappy-dev \
    zlib1g-dev \
    libbz2-dev \
    libsodium-dev \
    libelf-dev \
    libdwarf-dev \
    tzdata

  # Pin gflags and glog so apt cannot reinstall the broken system versions
  # (libboost or other transitive deps can silently pull them back in)
  sudo apt-mark hold libgflags-dev libgflags2.2 2>/dev/null || true
  sudo apt-get remove -y libgflags-dev libgoogle-glog-dev 2>/dev/null || true

  # ---------------------------------------------------------------------------
  # Step 2 — Build gflags from source WITH -fPIC
  #
  # The Ubuntu 24.04 ARM64 apt package of libgflags.a is compiled WITHOUT
  # -fPIC. When the linker tries to include it in libvelox.so it fails with:
  #   relocation R_AARCH64_ADR_PREL_PG_HI21 cannot be used when making a
  #   shared object; recompile with -fPIC
  # Building from source with CMAKE_POSITION_INDEPENDENT_CODE=ON fixes this.
  # We install to /usr/local so it takes precedence over /usr/lib at link time.
  # ---------------------------------------------------------------------------
  echo ""
  echo ">>> [2/5] Building gflags 2.2.2 from source with -fPIC..."
  cd /tmp
  rm -rf gflags-src
  git clone -b v2.2.2 --depth 1 https://github.com/gflags/gflags.git gflags-src
  cd gflags-src
  mkdir -p build && cd build
  cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DGFLAGS_BUILD_STATIC_LIBS=ON \
    -DGFLAGS_BUILD_SHARED_LIBS=ON \
    -DGFLAGS_NAMESPACE="google;gflags" \
    -DCMAKE_CXX_FLAGS="-fPIC" \
    -DCMAKE_INSTALL_PREFIX=/usr/local
  make -j"${NUM_THREADS}"
  sudo make install
  sudo ldconfig
  rm -rf /tmp/gflags-src
  echo "    gflags installed to /usr/local — verifying -fPIC..."
  # Confirm the .a has relocatable code (no absolute relocs)
  objdump -r /usr/local/lib/libgflags.a 2>/dev/null | grep -c "R_AARCH64_ADR_PREL" && \
    echo "WARNING: still seeing non-PIC relocations" || echo "    OK — gflags is -fPIC clean"

  # ---------------------------------------------------------------------------
  # Step 3 — Build glog from source (depends on gflags above)
  #
  # glog must be built AFTER gflags so it links our -fPIC gflags.
  # System libgoogle-glog-dev on Ubuntu 24.04 links against the broken apt
  # gflags, so we build glog from source too.
  # ---------------------------------------------------------------------------
  echo ""
  echo ">>> [3/5] Building glog 0.6.0 from source..."
  cd /tmp
  rm -rf glog-src
  git clone -b v0.6.0 --depth 1 https://github.com/google/glog.git glog-src
  cd glog-src
  mkdir -p build && cd build
  cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DWITH_UNWIND=OFF \
    -DBUILD_TESTING=OFF \
    -DCMAKE_CXX_FLAGS="-fPIC" \
    -DCMAKE_INSTALL_PREFIX=/usr/local
  make -j"${NUM_THREADS}"
  sudo make install
  sudo ldconfig
  rm -rf /tmp/glog-src
  echo "    glog installed to /usr/local"
fi   # end SKIP_SETUP=false

# ---------------------------------------------------------------------------
# Java / Maven environment
# ---------------------------------------------------------------------------
export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-17-openjdk-arm64}"
export PATH="${JAVA_HOME}/bin:${PATH}"
export CPU_TARGET="aarch64"
export NUM_THREADS="${NUM_THREADS}"

echo ""
echo ">>> Environment:"
echo "    GCC     : $(gcc --version | head -1)"
echo "    CMake   : $(cmake --version | head -1)"
echo "    Java    : $(java -version 2>&1 | head -1)"
echo "    Maven   : $(mvn -version 2>&1 | head -1)"
echo "    gflags  : $(pkg-config --modversion gflags 2>/dev/null || echo 'from /usr/local')"
echo "    CPU     : ${CPU_TARGET}"
echo "    Threads : ${NUM_THREADS}"

if [[ "${SETUP_ONLY}" == "true" ]]; then
  echo ""
  echo ">>> --setup-only specified. Dependencies installed. Exiting."
  exit 0
fi

# ---------------------------------------------------------------------------
# Step 4 — Build Velox + Arrow + Gluten CPP + Maven JAR
#
# buildbundle-veloxbe.sh does all four steps in order:
#   get_velox → setup_dependencies → build_arrow → build_velox → build_gluten_cpp → mvn install
#
# --run_setup_script=OFF because we already ran apt + built gflags/glog above.
# The Velox setup script would try to install system gflags/glog again and
# undo our -fPIC builds.
# ---------------------------------------------------------------------------
echo ""
echo ">>> [4/5] Building Velox + Arrow + Gluten CPP + Maven JAR..."
echo "    Spark version : ${SPARK_VERSION}"
echo "    This will take ~60-90 min on first run."
echo ""

cd "${SCRIPT_DIR}"
./dev/buildbundle-veloxbe.sh \
  --spark_version="${SPARK_VERSION}" \
  --build_type=Release \
  --run_setup_script=OFF \
  --num_threads="${NUM_THREADS}"

# ---------------------------------------------------------------------------
# Step 5 — Collect JARs
# ---------------------------------------------------------------------------
echo ""
echo ">>> [5/5] Collecting output JARs..."
mkdir -p "${OUTPUT_DIR}"
find "${SCRIPT_DIR}/package/target/" -name "*.jar" \
  ! -name "*sources*" ! -name "*javadoc*" \
  -exec cp -v {} "${OUTPUT_DIR}/" \; 2>/dev/null || true

echo ""
echo "============================================================"
echo " BUILD COMPLETE"
echo " JARs in: ${OUTPUT_DIR}"
echo "============================================================"
ls -lh "${OUTPUT_DIR}"/*.jar 2>/dev/null || echo "(no JARs found — check build output above)"
echo ""
echo "Deploy: copy the bundle JAR to your Spark cluster's extraClassPath"
echo "  e.g.: gluten-velox-bundle-spark3.5_2.12-*.jar"
