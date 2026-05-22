# Gluten + Velox Native Build — x86_64 Ubuntu 24.04

Complete guide for building the Gluten Velox backend on an AWS x86_64
instance (e.g. r6a.4xlarge / r6i.4xlarge, 16 vCPU, 128 GB RAM) running
Ubuntu 24.04 (Noble).

---

## Target Environment

| Item | Value |
|------|-------|
| Instance type | AWS r6a.4xlarge (AMD EPYC) or r6i.4xlarge (Intel) |
| Architecture | x86_64 |
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
./build-native-x86.sh
```

First run takes **60–90 minutes**. Output JARs land in `./output/`.

### Options

```bash
./build-native-x86.sh                  # full build, Spark 3.5, 16 threads
./build-native-x86.sh --threads=8      # override thread count
./build-native-x86.sh --spark=3.4      # different Spark version
./build-native-x86.sh --setup-only     # install deps only, then stop
./build-native-x86.sh --skip-setup     # skip dep install, go straight to build
```

---

## What `build-native-x86.sh` Does

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
| `gluten-velox-bundle-spark3.5_2.12-linux_amd64-1.7.0-SNAPSHOT.jar` | **Deploy this** — the all-in-one bundle |
| `gluten-package-1.7.0-SNAPSHOT.jar` | Equivalent uber JAR (same content) |
| `gluten-package-1.7.0-SNAPSHOT-3.5.jar` | Spark-version-tagged copy |
| `original-gluten-package-1.7.0-SNAPSHOT.jar` | Pre-shade Maven artifact (not needed) |

---

## Spark Deployment

Copy the bundle JAR to every node (driver + all workers):

```bash
scp gluten-velox-bundle-spark3.5_2.12-linux_amd64-1.7.0-SNAPSHOT.jar \
    spark-worker:/opt/spark/jars/
```

Add to `spark-defaults.conf` (or pass as `--conf` flags):

```
spark.plugins=org.apache.gluten.GlutenPlugin
spark.gluten.loadLibFromJar=true
spark.driver.extraClassPath=/opt/spark/jars/gluten-velox-bundle-spark3.5_2.12-linux_amd64-1.7.0-SNAPSHOT.jar
spark.executor.extraClassPath=/opt/spark/jars/gluten-velox-bundle-spark3.5_2.12-linux_amd64-1.7.0-SNAPSHOT.jar
```

---

## x86_64 Ubuntu 24.04 — Issues Fixed

Ubuntu 24.04 on x86_64 ships static libraries that are **not compiled with
`-fPIC`**. Although x86_64 relocation errors differ from ARM64, the root
cause and fixes are the same. Additionally, x86_64 has a unique runtime
symbol issue with Folly RTTI that ARM64 does not exhibit.

---

### Fix 1 — gflags not compiled with `-fPIC`

**Error (build time):**
```
relocation R_X86_64_32 against ... can not be used when making a shared object;
recompile with -fPIC
```
or, with Velox `BUNDLED` mode:
```
ld: _deps/gflags-build/libgflags.a: relocation error — recompile with -fPIC
```

**Root cause:** Two distinct sources of non-fPIC gflags:
1. The Ubuntu 24.04 x86_64 apt package `libgflags-dev` is not compiled with `-fPIC`.
2. When `gflags_SOURCE=BUNDLED`, Velox's internal FetchContent gflags build also
   lacked `-fPIC` until `-DCMAKE_POSITION_INDEPENDENT_CODE=ON` was added globally.
   Gluten CPP's own `FetchContent` gflags had the same problem.

**Fix:**
- Build gflags v2.2.2 from source with `-DCMAKE_POSITION_INDEPENDENT_CODE=ON`
  and install to `/usr/local` (done by `build-native-x86.sh` step 2/5).
- Set `gflags_SOURCE=BUNDLED` and pass `-DCMAKE_POSITION_INDEPENDENT_CODE=ON`
  in `build-velox.sh` so Velox's internal bundled gflags is also built with
  `-fPIC` (covers the Velox-side gflags).
- Gluten's own CMakeLists got `-DCMAKE_POSITION_INDEPENDENT_CODE=ON` propagated
  through the top-level cmake invocation (Fix 8 in this file also covers this).

**Files changed:** `build-native-x86.sh`, `ep/build-velox/src/build-velox.sh`

---

### Fix 2 — glog not compiled with `-fPIC`

**Error (build time):**
```
relocation R_X86_64_32 against symbol in libglog.a
```

**Root cause:** Ubuntu 24.04 x86_64 `libgoogle-glog-dev` apt package is not
compiled with `-fPIC`. Velox bundles glog internally but does not install it
to a path that Gluten CPP's `find_package(glog)` can discover.

**Fix:** Build glog v0.6.0 from source with `-fPIC` and install to `/usr/local`.

**Files changed:** `build-native-x86.sh`

---

### Fix 3 — `dwarf.h` not found

**Error (build time):**
```
fatal error: dwarf.h: No such file or directory
```

**Root cause:** Ubuntu 24.04 moved `libdwarf-dev` headers to
`/usr/include/libdwarf/dwarf.h`. Folly's `DwarfUtil.h` expects the classic
`<dwarf.h>` path. The `libdw-dev` package (elfutils) provides the header at
the expected location `/usr/include/dwarf.h`.

**Fix:** Add `libdw-dev` to the apt package list in `build-native-x86.sh`.

**Files changed:** `build-native-x86.sh`

---

### Fix 4 — protobuf not compiled with `-fPIC`

**Error (build time):**
```
/usr/lib/x86_64-linux-gnu/libprotobuf.a: relocation R_X86_64_32 against
symbol '...' can not be used when making a shared object; recompile with -fPIC
```

**Root cause:** Ubuntu 24.04 x86_64 `libprotobuf-dev` apt package is not
compiled with `-fPIC`. `build_gluten_cpp` in `dev/builddeps-veloxbe.sh`
installs it from apt on Debian systems right before running cmake. The linker
then rejects it when building `libgluten.so`.

**Fix:**
1. Build protobuf v3.21.12 from source with `-fPIC` in `build-native-x86.sh`
   setup (step 4/5) and install to `/usr/local`.
2. Remove and hold the system `libprotobuf-dev` package so apt cannot
   re-install the non-fPIC version.
3. In `build_gluten_cpp`, skip the apt protobuf install on x86_64 when
   `/usr/local/lib/libprotobuf.a` already exists from the source build.

**Files changed:** `build-native-x86.sh`, `dev/builddeps-veloxbe.sh`

---

### Fix 5 — Folly cmake config not found

**Error (cmake configure):**
```
CMake Error: Could not find a package configuration file provided by "Folly"
```

**Root cause:** Velox builds Folly via `FetchContent` with `EXCLUDE_FROM_ALL`
and applies `folly-no-export.patch` which removes Folly's install targets.
`FollyConfig.cmake` is never installed to a path Gluten CPP's cmake can find.

**Fix:** After Velox's build, install Folly's cmake config files with
`cmake --install folly-build/ || true` (the `|| true` skips the missing
`libfollybenchmark.a`). Then write a stub
`/usr/local/lib/cmake/folly/folly-targets.cmake` that defines `Folly::folly`
as an `INTERFACE IMPORTED` target — no actual library, because all Folly code
is compiled into `libvelox.a` via `VELOX_MONO_LIBRARY=ON`.

**Files changed:** `ep/build-velox/src/build-velox.sh`

---

### Fix 6 — `VELOX_ENABLE_GEO=OFF` causes cmake to abort

**Error (cmake configure):**
```
CMake Error: Library does not exist:
  .../velox_ep/_build/release/lib/libgeos.a
```

**Root cause:** Ubuntu 24.04 ships GEOS 3.12 which removed the
`CoordinateArraySequence` class that Velox's geo functions depend on.
`build-velox.sh` passes `-DVELOX_ENABLE_GEO=OFF`, so `libgeos.a` is never
built. But `cpp/velox/CMakeLists.txt` unconditionally called
`import_library(external::geos ...)` which `FATAL_ERROR`s if the file is missing.

**Fix:** Guard the GEOS import with an `EXISTS` check. If neither a system
GEOS package nor the bundled `libgeos.a` is present, skip silently.

**Files changed:** `cpp/velox/CMakeLists.txt`

---

### Fix 7 — Folly not found by Gluten CPP cmake

**Error (cmake configure):**
```
CMake Error: Could not find a package configuration file provided by "Folly"
  (required by VeloxBackend)
```

**Root cause:** Gluten CPP's cmake did not include the Velox build tree in
`CMAKE_PREFIX_PATH`, so `find_package(Folly REQUIRED CONFIG)` could not
locate the cmake config files installed in Fix 5.

**Fix:** Compute `VELOX_BUILD_PATH` from `VELOX_HOME` and build type, then
add it to `-DCMAKE_PREFIX_PATH` in the Gluten CPP cmake invocation.

**Files changed:** `dev/builddeps-veloxbe.sh`

---

### Fix 8 — Folly RTTI undefined symbols at JVM startup (x86_64 only)

**Error (runtime — JVM startup):**
```
java.lang.UnsatisfiedLinkError: .../libvelox.so:
  undefined symbol: _ZTIN5folly7futures6detail8CoreBaseE
```

**Confirmed with:**
```bash
# Extract libvelox.so from the old JAR
cd /tmp && mkdir gx-old && cd gx-old
jar -xf /path/to/old-jar.jar linux/amd64/
nm -D linux/amd64/libvelox.so | grep CoreBase
# Shows: U _ZTIN5folly7futures6detail8CoreBaseE  (U = undefined)
```

**Root cause:** `libvelox.so` is built by linking `libvelox.a` (the monolithic
Velox archive, ~300 MB) via `VELOX_MONO_LIBRARY=ON`. Without special linker
flags, the linker applies **dead-code elimination**: it only pulls in object
files from `libvelox.a` that define symbols directly or transitively referenced
by Gluten's bridge code. Folly RTTI typeinfo symbols (`_ZTI*`) for classes like
`folly::futures::detail::CoreBase` are needed at **runtime** (for
`dynamic_cast`, vtable dispatch, and exception handling involving Folly
Futures) but are not referenced at **link time** by Gluten's bridge layer.
On x86_64, the code paths taken by the linker don't happen to pull in the
`CoreBase` object file. On ARM64 they incidentally do, masking the bug.

**Symbols confirmed undefined in the old JAR:**
```
U _ZN5folly7futures6detail8CoreBase10setResult_EONS_17ExecutorKeepAliveINS_8ExecutorEEE
U _ZN5folly7futures6detail8CoreBase12setCallback_...
U _ZN5folly7futures6detail8CoreBase14destroyDerivedEv
U _ZN5folly7futures6detail8CoreBase21stealDeferredExecutorEv
U _ZN5folly7futures6detail8CoreBase28initCopyInterruptHandlerFromERKS2_
U _ZN5folly7futures6detail8CoreBase5raiseENS_17exception_wrapperE
U _ZN5folly7futures6detail8CoreBase9detachOneEv
U _ZN5folly7futures6detail8CoreBaseD2Ev
U _ZNK5folly7futures6detail8CoreBase9hasResultEv
U _ZTIN5folly7futures6detail8CoreBaseE
```

**Fix:** Add `--whole-archive`/`--no-whole-archive` around `libvelox.a` in the
`libvelox.so` link step. This forces the linker to include **all** object files
from `libvelox.a`, ensuring every Folly RTTI typeinfo, vtable, and function
symbol is present in `libvelox.so` regardless of whether Gluten directly
references it.

```cmake
# cpp/velox/CMakeLists.txt
import_library(facebook::velox ${VELOX_BUILD_PATH}/lib/libvelox.a)

if(NOT CMAKE_SYSTEM_NAME MATCHES "Darwin")
  target_link_options(velox PRIVATE
    "-Wl,--whole-archive" "$<TARGET_FILE:facebook::velox>" "-Wl,--no-whole-archive")
endif()
```

The `$<TARGET_FILE:facebook::velox>` generator expression expands to the actual
path of `libvelox.a` at link time, so the flag is applied correctly. The Darwin
guard is needed because macOS uses `-force_load` instead of `--whole-archive`.

**Verify the fix after rebuild:**
```bash
cd /tmp && rm -rf gx-new && mkdir gx-new && cd gx-new
jar -xf ~/gluten-velox/output/gluten-velox-bundle-spark3.5_2.12-linux_amd64-*.jar linux/amd64/
nm -D linux/amd64/libvelox.so | grep "_ZTIN5folly7futures6detail8CoreBaseE"
# Must show W (weak, defined) — not U (undefined)
```

**Files changed:** `cpp/velox/CMakeLists.txt`

**Commit:** `81f9cfbc7`

---

## Auditing the Old (Broken) JAR

If you have the old JAR and want to check for **all** undefined non-OS symbols
(not just CoreBase):

```bash
cd /tmp && rm -rf gx-audit && mkdir gx-audit && cd gx-audit
jar -xf /path/to/old-gluten-velox-bundle-spark3.5_2.12-linux_amd64-*.jar linux/amd64/

# 1. All undefined symbols that are NOT from standard glibc/libstdc++/libgcc
nm -D linux/amd64/libvelox.so | awk '$2 == "U"' \
  | grep -vE '@(GLIBC_|CXXABI_|GCC_|GLIBCXX_)' \
  | sort -k3

# 2. Specifically: undefined RTTI typeinfo (_ZTI) and vtables (_ZTV) — these
#    cause UnsatisfiedLinkError at JVM startup or first use
nm -D linux/amd64/libvelox.so | awk '$2 == "U"' \
  | grep -E '_ZTI|_ZTV'

# 3. Folly-specific undefined symbols
nm -D linux/amd64/libvelox.so | awk '$2 == "U"' \
  | grep -i folly

# 4. Compare libgluten.so as well
nm -D linux/amd64/libgluten.so | awk '$2 == "U"' \
  | grep -vE '@(GLIBC_|CXXABI_|GCC_|GLIBCXX_)' \
  | sort -k3
```

The `--whole-archive` fix (Fix 8) addresses ALL Folly RTTI and function symbol
omissions in one shot — once applied, `libvelox.so` contains every symbol from
`libvelox.a` so no further undefined-symbol surprises are expected.

---

## Incremental Rebuilds

If the build fails partway through, use `--skip-setup` to avoid re-building
gflags/glog/protobuf from source (they are already installed):

```bash
./build-native-x86.sh --skip-setup
```

If the Gluten CPP cmake cache is poisoned (e.g. after changing cmake options
or fixing a library that cmake already cached a bad path for):

```bash
rm -rf cpp/build/
./build-native-x86.sh --skip-setup
```

If Velox itself needs to be rebuilt from scratch:

```bash
rm -rf ep/build-velox/build/
./build-native-x86.sh --skip-setup
```

**Wiping `cpp/build/` alone is safe and fast (~15–20 min) when only Gluten CPP
or cmake config files changed.** Only wipe `ep/build-velox/build/` when Velox
source or build flags changed (~60 min to rebuild).

---

## Copying the JAR to Local Machine

```bash
# From your local machine
scp aws-x86-ec2:/home/ubuntu/gluten-velox/output/*.jar ~/Downloads/gluten-velox/
```

The file to deploy:
```
gluten-velox-bundle-spark3.5_2.12-linux_amd64-1.7.0-SNAPSHOT.jar
```
