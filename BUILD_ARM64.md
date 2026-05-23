# Gluten + Velox Native Build — ARM64 Ubuntu 24.04

Complete guide for building the Gluten Velox backend on an AWS Graviton3
instance (r7g.4xlarge, 16 vCPU, 128 GB RAM) running Ubuntu 24.04 (Noble).

---

## Target Environment

| Item | Value |
|------|-------|
| Instance type | AWS r7g.4xlarge (Graviton3) |
| Architecture | aarch64 (ARM64) |
| OS | Ubuntu 24.04 LTS (Noble) |
| RAM | 128 GB |
| vCPU | 16 |
| Java | OpenJDK 17 |
| GCC | 13 (Ubuntu default) |
| Spark | 3.5 |

---

## Quick Start

```bash
git clone https://github.com/sp-202/gluten-velox.git
cd gluten-velox
./build-native.sh
```

First run takes **60–90 minutes**. Output JARs land in `./output/`.

### Options

```bash
./build-native.sh                  # full build, Spark 3.5, 16 threads
./build-native.sh --threads=8      # override thread count
./build-native.sh --spark=3.4      # different Spark version
./build-native.sh --setup-only     # install deps only, then stop
./build-native.sh --skip-setup     # skip dep install, go straight to build
```

---

## What `build-native.sh` Does

The script is a self-contained orchestrator that runs five steps in order:

| Step | Action |
|------|--------|
| 1/5 | Install system packages via apt |
| 2/5 | Build **gflags** v2.2.2 from source (with `-fPIC`) |
| 3/5 | Build **glog** v0.6.0 from source (with `-fPIC`) |
| 4/5 | Build **protobuf** v3.21.12 from source (with `-fPIC`) |
| 5/5 | Build Velox + Arrow + Gluten CPP + Maven JAR |

Step 5 calls `dev/buildbundle-veloxbe.sh` which internally runs:
1. `get-velox.sh` — clones the pinned Velox commit
2. Arrow build — builds Apache Arrow C++ from Velox's bundled copy
3. `build-velox.sh` — compiles Velox into `libvelox.a` (mono library)
4. `build_gluten_cpp` — compiles Gluten CPP into `libgluten.so` + `libvelox.so`
5. `mvn install` — packages everything into the bundle JAR

---

## Output JARs

After a successful build, `./output/` contains:

| File | Purpose |
|------|---------|
| `gluten-velox-bundle-spark3.5_2.12-linux_aarch64-1.7.0-SNAPSHOT.jar` | **Deploy this** — the all-in-one bundle (91 MB) |
| `gluten-package-1.7.0-SNAPSHOT.jar` | Equivalent uber JAR (same content) |
| `gluten-package-1.7.0-SNAPSHOT-3.5.jar` | Spark-version-tagged copy |
| `original-gluten-package-1.7.0-SNAPSHOT.jar` | Pre-shade Maven artifact (not needed) |

---

## Spark Deployment

Copy the bundle JAR to every node (driver + all workers):

```bash
scp gluten-velox-bundle-spark3.5_2.12-linux_aarch64-1.7.0-SNAPSHOT.jar \
    spark-worker:/opt/spark/jars/
```

Add to `spark-defaults.conf` (or pass as `--conf` flags):

```
spark.plugins=org.apache.gluten.GlutenPlugin
spark.gluten.loadLibFromJar=true
spark.driver.extraClassPath=/opt/spark/jars/gluten-velox-bundle-spark3.5_2.12-linux_aarch64-1.7.0-SNAPSHOT.jar
spark.executor.extraClassPath=/opt/spark/jars/gluten-velox-bundle-spark3.5_2.12-linux_aarch64-1.7.0-SNAPSHOT.jar
```

---

## ARM64 Ubuntu 24.04 — Issues Fixed

Ubuntu 24.04 on ARM64 (aarch64) ships several static libraries that are **not
compiled with `-fPIC`**. When any of these are linked into a shared library
(`.so`), the linker raises:

```
relocation R_AARCH64_ADR_PREL_PG_HI21 against symbol '...' which may bind
externally can not be used when making a shared object; recompile with -fPIC
```

The following sections document each issue encountered and how it was fixed.

---

### Fix 1 — gflags not compiled with `-fPIC`

**Error:**
```
R_AARCH64_ADR_PREL_PG_HI21 relocation error in libgflags.a
```

**Root cause:** Ubuntu 24.04 ARM64 `libgflags-dev` apt package does not have
`-fPIC`. Velox's cmake also has a `BUNDLED` path for gflags but it requires
the 'shared' component which is absent on ARM64.

**Fix:** Build gflags v2.2.2 from source with
`-DCMAKE_POSITION_INDEPENDENT_CODE=ON` and install to `/usr/local`. Set
`gflags_SOURCE=BUNDLED` in `build-velox.sh` so Velox uses its own internal
copy during its own build, while the system path (for Gluten CPP) gets the
source-built one.

**Files changed:** `build-native.sh`, `ep/build-velox/src/build-velox.sh`

---

### Fix 2 — glog not compiled with `-fPIC`

**Error:**
```
CMake Error: Could NOT find glog (missing: GLOG_LIBRARY)
```
then linker error when attempting to link the system `libglog.a`.

**Root cause:** Ubuntu 24.04 ARM64 `libgoogle-glog-dev` apt package lacks
`-fPIC`. Velox bundles glog internally but does not install it to a path
that Gluten CPP's `find_package(glog)` can discover.

**Fix:** Build glog v0.6.0 from source with `-fPIC` and install to
`/usr/local` so Gluten CPP can find it via the standard cmake search path.

**Files changed:** `build-native.sh`

---

### Fix 3 — `dwarf.h` not found

**Error:**
```
fatal error: dwarf.h: No such file or directory
```

**Root cause:** Ubuntu 24.04 moved `libdwarf-dev` headers to
`/usr/include/libdwarf/dwarf.h`. Folly's `DwarfUtil.h` expects the classic
`<dwarf.h>` path. The `libdw-dev` package (elfutils) provides the header at
the expected location `/usr/include/dwarf.h`.

**Fix:** Add `libdw-dev` to the apt package list in `build-native.sh`.

**Files changed:** `build-native.sh`

---

### Fix 4 — `gflags/gflags.h` not found in dbgen target

**Error:**
```
fatal error: gflags/gflags.h: No such file or directory
```
in Velox's `dbgen` cmake target.

**Root cause:** The `dbgen` target does not inherit Velox's internal bundled
gflags include path. This appeared after the system gflags package was
removed and before the from-source install was in place.

**Fix:** Covered by Fix 1 — once gflags is installed from source to
`/usr/local/include/gflags/`, both the Velox internal build (via
`gflags_SOURCE=BUNDLED`) and external targets (via the system include path)
find the headers correctly.

---

### Fix 5 — Folly cmake config not found

**Error:**
```
CMake Error: Could not find a package configuration file provided by "Folly"
```

**Root cause:** Velox builds Folly via `FetchContent` with `EXCLUDE_FROM_ALL`
and applies `folly-no-export.patch` which removes Folly's install targets.
`FollyConfig.cmake` is never installed to a path Gluten CPP's cmake can find,
and `OVERRIDE_FIND_PACKAGE` only works within the same cmake invocation.

**Fix:** After Velox's build, install Folly's cmake config files with
`cmake --install folly-build/ || true` (the `|| true` skips the missing
`libfollybenchmark.a` which is not built by default). Then write a stub
`/usr/local/lib/cmake/folly/folly-targets.cmake` that defines `Folly::folly`
as an `INTERFACE IMPORTED` target — no actual library needed because all
Folly code is compiled into `libvelox.a` via `VELOX_MONO_LIBRARY=ON`.

**Files changed:** `ep/build-velox/src/build-velox.sh`

---

### Fix 6 — `VELOX_ENABLE_GEO=OFF` causes cmake to abort

**Error:**
```
CMake Error: Library does not exist:
  .../velox_ep/_build/release/lib/libgeos.a
```

**Root cause:** Ubuntu 24.04 ships GEOS 3.12 which removed the
`CoordinateArraySequence` class that Velox's geo functions depend on.
`build-velox.sh` therefore passes `-DVELOX_ENABLE_GEO=OFF`, so `libgeos.a`
is never built. But `cpp/velox/CMakeLists.txt` unconditionally called
`import_library(external::geos ...)` which `FATAL_ERROR`s if the file is
missing.

**Fix:** Guard the GEOS import in `cpp/velox/CMakeLists.txt` with an
`EXISTS` check. If neither a system GEOS package nor the bundled
`libgeos.a` is present, skip the link silently with a `STATUS` message.

**Files changed:** `cpp/velox/CMakeLists.txt`

---

### Fix 7 — Folly not found by Gluten CPP cmake

**Error:**
```
CMake Error: Could not find a package configuration file provided by "Folly"
  (required by VeloxBackend)
```

**Root cause:** Gluten CPP's cmake (`build_gluten_cpp` in
`dev/builddeps-veloxbe.sh`) did not include the Velox build tree in
`CMAKE_PREFIX_PATH`, so `find_package(Folly REQUIRED CONFIG)` could not
locate the cmake config files we installed in Fix 5.

**Fix:** Compute `VELOX_BUILD_PATH` from `VELOX_HOME` and build type, then
add it to `-DCMAKE_PREFIX_PATH` in the Gluten CPP cmake invocation.

**Files changed:** `dev/builddeps-veloxbe.sh`

---

### Fix 8 — protobuf not compiled with `-fPIC`

**Error:**
```
/usr/lib/aarch64-linux-gnu/libprotobuf.a(proto_writer.o): relocation
R_AARCH64_ADR_PREL_PG_HI21 against symbol '...' can not be used when
making a shared object; recompile with -fPIC
```

**Root cause:** Ubuntu 24.04 ARM64 `libprotobuf-dev` apt package is not
compiled with `-fPIC`. `build_gluten_cpp` in `dev/builddeps-veloxbe.sh`
installs it from apt on Debian systems right before running cmake. The linker
then rejects it when building `libgluten.so`.

**Fix:**
1. Build protobuf v3.21.12 from source with `-fPIC` in `build-native.sh`
   setup (Step 4/5) and install to `/usr/local`.
2. Remove and hold the system `libprotobuf-dev` package so apt cannot
   re-install the non-fPIC version.
3. In `build_gluten_cpp`, skip the apt protobuf install on `aarch64` —
   the from-source install at `/usr/local` is used instead.

**Files changed:** `build-native.sh`, `dev/builddeps-veloxbe.sh`

---

### Fix 9 — `SharedLibraryLoaderUbuntu2404` missing — JVM cannot find loader

**Error (runtime — JVM startup on fresh Ubuntu 24.04 node):**
```
org.apache.gluten.exception.GlutenException: Cannot find SharedLibraryLoader
for Ubuntu 24.04.4 LTS (Noble Numbat), please check whether your custom
SharedLibraryLoader is implemented and loadable.
```

**Root cause:** The old ARM64 JAR in S3 was built before
`SharedLibraryLoaderUbuntu2404` was added to the source. The SPI mechanism
iterates all registered loaders and calls `accepts(osName, osVersion)` — none
of the old loaders matched `"Ubuntu 24.04"`, so Gluten aborted.

**Fix:** Add `SharedLibraryLoaderUbuntu2404.scala` and register it in
`META-INF/services/org.apache.gluten.spi.SharedLibraryLoader`. The new loader
matches `osName.contains("Ubuntu") && osVersion.startsWith("24.04")` and only
loads the 4 `.so` files actually present in the JAR — system libs are resolved
automatically at runtime via ELF `DT_NEEDED` entries in `libvelox.so`.

**Files changed:** `backends-velox/src/main/scala/org/apache/gluten/spi/SharedLibraryLoaderUbuntu2404.scala`,
`backends-velox/src/main/resources/META-INF/services/org.apache.gluten.spi.SharedLibraryLoader`

---

### Fix 10 — Wrong Arrow JNI path inside JAR (`aarch64/` vs `aarch_64/`)

**Error (runtime — JVM startup):**
```
org.apache.gluten.exception.GlutenException:
  java.io.FileNotFoundException: aarch64/libarrow_cdata_jni.so
```

**Root cause:** Arrow's Maven build places JNI libs at `aarch_64/` (with an
underscore between `aarch` and `64`) inside the JAR on ARM64. The initial
`SharedLibraryLoaderUbuntu2404` loader used `aarch64/` (no underscore), which
does not exist in the JAR.

Confirmed from `jar -tf` output:
```
aarch_64/libarrow_cdata_jni.so
aarch_64/libarrow_dataset_jni.so
linux/aarch64/libgluten.so
linux/aarch64/libvelox.so
```

**Fix:** Use `aarch_64/` for Arrow JNI prefix on ARM64. The loader detects
`os.arch` at runtime and selects the correct prefix pair:

```scala
val arch = System.getProperty("os.arch", "")
val (arrowPrefix, glutenPrefix) = if (arch == "aarch64") {
  ("aarch_64", "linux/aarch64")   // ARM64: note underscore in aarch_64
} else {
  ("x86_64", "linux/amd64")       // x86_64
}
```

**Files changed:** `backends-velox/src/main/scala/org/apache/gluten/spi/SharedLibraryLoaderUbuntu2404.scala`

---

## Runtime Dependencies (Fresh Ubuntu 24.04 ARM64 Node)

Every node that runs the Gluten JAR (driver + all workers) must have these
system packages installed. The JAR bundles only `libarrow_cdata_jni.so`,
`libarrow_dataset_jni.so`, `libgluten.so`, and `libvelox.so` — all other
libraries are resolved at runtime from the system via ELF `DT_NEEDED` entries
embedded in `libvelox.so`.

```bash
sudo apt-get update && sudo apt-get install -y \
  openjdk-17-jdk \
  libboost-filesystem1.83.0 \
  libboost-program-options1.83.0 \
  libboost-context1.83.0 \
  libsnappy1v5 \
  liblz4-1 \
  libzstd1 \
  libbz2-1.0 \
  libssl3t64 \
  libfmt9 \
  libdouble-conversion3 \
  libevent-2.1-7t64 \
  libsodium23 \
  liblzma5 \
  libre2-10
```

**Ubuntu 24.04 package name changes vs older releases:**

| Old name (Ubuntu 22.04 and earlier) | Ubuntu 24.04 name |
|-------------------------------------|-------------------|
| `libsnappy1` | `libsnappy1v5` |
| `libssl3` | `libssl3t64` |
| `libevent-2.1-7` | `libevent-2.1-7t64` |
| `libre2-9` | `libre2-10` |

---

## Fresh EC2 Deployment (from S3)

To deploy on a fresh ARM64 Ubuntu 24.04 node without building from source:

**Step 1 — Install runtime dependencies (see above)**

**Step 2 — Install Spark 3.5:**
```bash
wget https://archive.apache.org/dist/spark/spark-3.5.3/spark-3.5.3-bin-hadoop3.tgz
tar -xzf spark-3.5.3-bin-hadoop3.tgz
export SPARK_HOME=$HOME/spark-3.5.3-bin-hadoop3
export PATH=$SPARK_HOME/bin:$PATH
```

**Step 3 — Pull the JAR from S3:**
```bash
aws s3 cp s3://gluten-jars-2026/gluten/arm64/gluten-velox-bundle-spark3.5_2.12-linux_aarch64-1.7.0-SNAPSHOT.jar \
  $SPARK_HOME/jars/
```

**Step 4 — Launch and verify:**
```bash
$SPARK_HOME/bin/spark-shell \
  --master local[4] \
  --driver-memory 8g \
  --conf spark.plugins=org.apache.gluten.GlutenPlugin \
  --conf spark.gluten.loadLibFromJar=true \
  --conf spark.memory.offHeap.enabled=true \
  --conf spark.memory.offHeap.size=24g \
  --conf spark.sql.adaptive.enabled=false \
  --conf spark.shuffle.manager=org.apache.spark.shuffle.sort.ColumnarShuffleManager \
  --jars $SPARK_HOME/jars/gluten-velox-bundle-spark3.5_2.12-linux_aarch64-1.7.0-SNAPSHOT.jar
```

Inside spark-shell, confirm Velox is active:
```scala
spark.sql("SELECT sum(id), count(*) FROM range(10000000)").explain()
```

Look for `VeloxColumnarToRow` and `HashAggregateTransformer` in the plan.

---

## Uploading Built JARs to S3

After a successful build, extract the `.so` files and upload everything:

```bash
# Extract .so files from the JAR
cd ~
mkdir -p so-files
cd so-files
jar -xf ~/gluten-velox/output/gluten-velox-bundle-spark3.5_2.12-linux_aarch64-1.7.0-SNAPSHOT.jar \
  linux/aarch64/libgluten.so \
  linux/aarch64/libvelox.so \
  aarch_64/libarrow_cdata_jni.so \
  aarch_64/libarrow_dataset_jni.so

# Upload JARs
aws s3 cp ~/gluten-velox/output/gluten-velox-bundle-spark3.5_2.12-linux_aarch64-1.7.0-SNAPSHOT.jar s3://gluten-jars-2026/gluten/arm64/
aws s3 cp ~/gluten-velox/output/gluten-package-1.7.0-SNAPSHOT.jar s3://gluten-jars-2026/gluten/arm64/
aws s3 cp ~/gluten-velox/output/gluten-package-1.7.0-SNAPSHOT-3.5.jar s3://gluten-jars-2026/gluten/arm64/
aws s3 cp ~/gluten-velox/output/original-gluten-package-1.7.0-SNAPSHOT.jar s3://gluten-jars-2026/gluten/arm64/

# Upload .so files
aws s3 cp ~/so-files/linux/aarch64/libgluten.so s3://gluten-jars-2026/gluten/arm64/so/
aws s3 cp ~/so-files/linux/aarch64/libvelox.so s3://gluten-jars-2026/gluten/arm64/so/
aws s3 cp ~/so-files/aarch_64/libarrow_cdata_jni.so s3://gluten-jars-2026/gluten/arm64/so/
aws s3 cp ~/so-files/aarch_64/libarrow_dataset_jni.so s3://gluten-jars-2026/gluten/arm64/so/
```

Note: `jar -xf` must be run from a writable directory (not from inside `output/`
which is owned by root). Run from `$HOME` instead.

---

## Incremental Rebuilds

If the build fails partway through, use `--skip-setup` to avoid re-building
gflags/glog/protobuf from source (they are already installed):

```bash
./build-native.sh --skip-setup
```

If the Gluten CPP cmake cache is poisoned (e.g. after changing cmake options
or fixing a library that cmake already cached a bad path for):

```bash
rm -rf cpp/build/
./build-native.sh --skip-setup
```

If Velox itself needs to be rebuilt from scratch:

```bash
rm -rf ep/build-velox/build/
./build-native.sh --skip-setup
```

---

## Copying the JAR to Local Machine

If your SSH config has a host alias (e.g. `aws-test-ec2`):

```bash
# From your local machine
scp aws-test-ec2:/home/ubuntu/gluten-velox/output/*.jar ~/Downloads/gluten-velox/
```

The file you need is:
```
gluten-velox-bundle-spark3.5_2.12-linux_aarch64-1.7.0-SNAPSHOT.jar
```
