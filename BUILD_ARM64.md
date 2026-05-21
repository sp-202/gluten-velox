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
