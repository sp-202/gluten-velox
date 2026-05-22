#!/usr/bin/env bash
# =============================================================================
# build-native-x86.sh — Native Gluten + Velox build for x86_64 Ubuntu 24.04
# Target: AWS r6a.4xlarge (AMD EPYC, 16 vCPU, 128 GB RAM)
#
# Usage:
#   ./build-native-x86.sh              # full build, Spark 3.5, 16 threads
#   ./build-native-x86.sh --threads=8  # override thread count
#   ./build-native-x86.sh --spark=3.4  # different Spark version
#   ./build-native-x86.sh --setup-only # install deps and stop
#   ./build-native-x86.sh --skip-setup # skip apt/gflags/glog/protobuf, go straight to build
#
# Run as a non-root user that has passwordless sudo, OR as root.
# Output JAR lands in ./output/
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTPUT_DIR="${SCRIPT_DIR}/output"

# ---------------------------------------------------------------------------
# Defaults — tuned for r6a.4xlarge (16 vCPU / 128 GB)
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
# Verify we are on x86_64 Ubuntu 24.04
# ---------------------------------------------------------------------------
ARCH="$(uname -m)"
if [[ "${ARCH}" != "x86_64" ]]; then
  echo "ERROR: This script targets x86_64. Detected: ${ARCH}"
  exit 1
fi

echo "============================================================"
echo " Gluten + Velox Native Build  —  x86_64 Ubuntu 24.04"
echo " Arch    : ${ARCH}"
echo " Spark   : ${SPARK_VERSION}"
echo " Threads : ${NUM_THREADS}"
echo " Output  : ${OUTPUT_DIR}"
echo "============================================================"

# ---------------------------------------------------------------------------
# Step 1 — System packages
#
# Ubuntu 24.04 x86_64 static libs (gflags, glog, protobuf) from apt are NOT
# compiled with -fPIC — same issue as ARM64.  Linking them into libgluten.so
# causes relocation errors.  We build all three from source below.
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
    libtool autoconf automake bison flex libfl-dev \
    python3 python3-pip python3-venv \
    openjdk-17-jdk \
    maven \
    libboost-all-dev \
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
    libdw-dev \
    tzdata

  # Remove system gflags/glog/protobuf — apt x86_64 packages lack -fPIC and
  # cause relocation errors when linking into libgluten.so / libvelox.so.
  # Hold them so inner apt-get calls in builddeps-veloxbe.sh can't re-install.
  sudo apt-get remove -y \
    libgflags-dev libgflags2.2 \
    libgoogle-glog-dev \
    libprotobuf-dev libprotobuf-lite23 protobuf-compiler 2>/dev/null || true
  sudo apt-mark hold \
    libgflags-dev libgflags2.2 \
    libprotobuf-dev libprotobuf-lite23 protobuf-compiler 2>/dev/null || true

  # ---------------------------------------------------------------------------
  # Step 2 — Build gflags from source with -fPIC
  # ---------------------------------------------------------------------------
  echo ""
  echo ">>> [2/5] Building gflags from source (with -fPIC)..."
  rm -rf /tmp/gflags-src /tmp/gflags-build
  git clone --depth=1 --branch=v2.2.2 https://github.com/gflags/gflags.git /tmp/gflags-src
  cmake -S /tmp/gflags-src -B /tmp/gflags-build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_TESTING=OFF
  cmake --build /tmp/gflags-build -j"${NUM_THREADS}"
  sudo cmake --install /tmp/gflags-build

  # ---------------------------------------------------------------------------
  # Step 3 — Build glog from source with -fPIC
  # ---------------------------------------------------------------------------
  echo ""
  echo ">>> [3/5] Building glog from source (with -fPIC)..."
  rm -rf /tmp/glog-src /tmp/glog-build
  git clone --depth=1 --branch=v0.6.0 https://github.com/google/glog.git /tmp/glog-src
  cmake -S /tmp/glog-src -B /tmp/glog-build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DWITH_GTEST=OFF
  cmake --build /tmp/glog-build -j"${NUM_THREADS}"
  sudo cmake --install /tmp/glog-build

  # ---------------------------------------------------------------------------
  # Step 4 — Build protobuf from source with -fPIC
  # Ubuntu 24.04 x86_64 libprotobuf.a lacks -fPIC; linking into libgluten.so
  # causes relocation errors.  builddeps-veloxbe.sh skips apt protobuf on
  # x86_64 when /usr/local/lib/libprotobuf.a already exists (see guard there).
  # ---------------------------------------------------------------------------
  echo ""
  echo ">>> [4/5] Building protobuf from source (with -fPIC)..."
  rm -rf /tmp/protobuf-src /tmp/protobuf-build
  git clone --depth=1 --branch=v3.21.12 \
    https://github.com/protocolbuffers/protobuf.git /tmp/protobuf-src
  cmake -S /tmp/protobuf-src -B /tmp/protobuf-build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -Dprotobuf_BUILD_TESTS=OFF \
    -Dprotobuf_BUILD_EXAMPLES=OFF
  cmake --build /tmp/protobuf-build -j"${NUM_THREADS}"
  sudo cmake --install /tmp/protobuf-build

fi   # end SKIP_SETUP=false

# ---------------------------------------------------------------------------
# Java / Maven environment
# ---------------------------------------------------------------------------
export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-17-openjdk-amd64}"
export PATH="${JAVA_HOME}/bin:${PATH}"
# Do NOT set CPU_TARGET for x86_64 — valid values are avx/sse/aarch64/arm64/ppc64le.
# Leaving it unset lets build-helper-functions.sh auto-detect AVX/SSE from /proc/cpuinfo.
export NUM_THREADS="${NUM_THREADS}"

echo ""
echo ">>> Environment:"
echo "    GCC     : $(gcc --version | head -1)"
echo "    CMake   : $(cmake --version | head -1)"
echo "    Java    : $(java -version 2>&1 | head -1)"
echo "    Maven   : $(mvn -version 2>&1 | head -1)"
echo "    gflags  : $(pkg-config --modversion gflags 2>/dev/null || echo 'from /usr/local')"
echo "    CPU     : auto-detect (avx/sse from /proc/cpuinfo)"
echo "    Threads : ${NUM_THREADS}"

if [[ "${SETUP_ONLY}" == "true" ]]; then
  echo ""
  echo ">>> --setup-only specified. Dependencies installed. Exiting."
  exit 0
fi

# ---------------------------------------------------------------------------
# Wipe stale Folly cmake install
# If ep/build-velox/build/ was deleted and re-created, the previously
# installed folly-targets.cmake at /usr/local/lib/cmake/folly/ still points
# at _deps/folly-src inside the old (now deleted) build tree. Velox then
# finds this broken system Folly instead of building its own. Remove it now
# so Velox builds Folly fresh and installs a correct targets file.
# ---------------------------------------------------------------------------
sudo rm -rf /usr/local/lib/cmake/folly

# ---------------------------------------------------------------------------
# Step 5 — Build Velox + Arrow + Gluten CPP + Maven JAR
# ---------------------------------------------------------------------------
echo ""
echo ">>> [5/5] Building Velox + Arrow + Gluten CPP + Maven JAR..."
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
# Collect JARs
# ---------------------------------------------------------------------------
echo ""
echo ">>> Collecting output JARs..."
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
