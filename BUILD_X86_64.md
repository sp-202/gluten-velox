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

**Scope of the problem — the old JAR has hundreds of undefined Folly symbols,
not just CoreBase.** CoreBase was simply the first crash because
`_ZTIN5folly7futures6detail8CoreBaseE` is a RTTI *data* (typeinfo) symbol,
which the dynamic linker resolves **eagerly** at `dlopen` time even under the
default `RTLD_LAZY` mode. All the other undefined Folly *function* symbols
would have caused additional crashes the first time those code paths were
exercised during query execution.

Audit of the old JAR confirmed undefined Folly symbol groups (representative
subset — full `nm -D | grep ' U ' | grep folly` output had 100+ entries):

```
# Folly Futures / CoreBase — crash at JVM startup (data/RTTI, eagerly resolved)
U _ZTIN5folly7futures6detail8CoreBaseE
U _ZN5folly7futures6detail8CoreBase10setResult_...
U _ZN5folly7futures6detail8CoreBase12setCallback_...
U _ZN5folly7futures6detail8CoreBase14destroyDerivedEv
U _ZN5folly7futures6detail8CoreBase21stealDeferredExecutorEv
U _ZN5folly7futures6detail8CoreBase9detachOneEv
U _ZN5folly7futures6detail8CoreBaseD2Ev

# Folly SharedMutex internals — crash on first lock/unlock
U _ZN5folly15SharedMutexImplILb0E...13unlock_sharedEv
U _ZN5folly15SharedMutexImplILb0E...6unlockEv
U _ZN5folly15SharedMutexImplILb1E...32tryUnlockTokenlessSharedDeferredEv

# Folly ThreadLocal internals
U _ZN5folly18threadlocal_detail14StaticMetaBase22allocateNewThreadEntryEv
U _ZN5folly18threadlocal_detail14StaticMetaBase7destroyEPNS1_7EntryIDE
U _ZN5folly18threadlocal_detail14StaticMetaBase8allocateEPNS1_7EntryIDE

# Folly Executor (IOThreadPoolExecutor / CPUThreadPoolExecutor)
U _ZN5folly20IOThreadPoolExecutorC1EmSt10shared_ptrINS_13ThreadFactoryEE...
U _ZN5folly21CPUThreadPoolExecutorC1EmNS0_7OptionsE

# Folly Fibers / Baton
U _ZN5folly6fibers5Baton4postEv
U _ZN5folly6fibers5Baton4waitEv
U _ZN5folly6fibers5Fiber6resumeEv

# Folly IOBuf
U _ZN5folly5IOBuf6createEm
U _ZN5folly5IOBufC1ENS0_8CreateOpEm
U _ZN5folly5IOBufD1Ev

# Folly Singleton / logging / misc
U _ZN5folly14SingletonVaultC1ENS0_4TypeE
U _ZN5folly14SingletonVault24scheduleDestroyInstancesEv
U _ZN5folly18LogStreamProcessorC1E...
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

### Fix 9 — glog and gflags RTTI symbols also missing from `libvelox.so`

**Error (runtime — JVM startup, after Fix 8):**
```
java.lang.UnsatisfiedLinkError: .../libvelox.so:
  undefined symbol: _ZTIN6google12LogMessageERE  (glog)
  undefined symbol: _ZTIN8gflags26CommandLineFlagInfoE      (gflags)
```

**Root cause:** Fix 8 added `--whole-archive` for `libvelox.a` (Folly). But
glog and gflags are also compiled as separate static libraries inside the Velox
build tree — `_deps/glog-build/libglog.a` and
`_deps/gflags-build/libgflags_nothreads.a`. Their RTTI typeinfo and function
symbols are referenced from Velox's code but were not included in `libvelox.a`.
Without `--whole-archive` on these two archives as well, the linker omits them.

**Fix:** Extend the `--whole-archive` block in `cpp/velox/CMakeLists.txt` to
also cover `libglog.a` and `libgflags_nothreads.a` from the Velox build tree:

```cmake
# cpp/velox/CMakeLists.txt
set(GLOG_STATIC_LIB  "${VELOX_BUILD_PATH}/_deps/glog-build/libglog.a")
set(GFLAGS_STATIC_LIB "${VELOX_BUILD_PATH}/_deps/gflags-build/libgflags_nothreads.a")

if(EXISTS "${GLOG_STATIC_LIB}")
  import_library(facebook::glog_static ${GLOG_STATIC_LIB})
endif()
if(EXISTS "${GFLAGS_STATIC_LIB}")
  import_library(facebook::gflags_static ${GFLAGS_STATIC_LIB})
endif()

if(NOT CMAKE_SYSTEM_NAME MATCHES "Darwin")
  set(_wa_libs "$<TARGET_FILE:facebook::velox>")
  if(EXISTS "${GLOG_STATIC_LIB}")
    list(APPEND _wa_libs "$<TARGET_FILE:facebook::glog_static>")
  endif()
  if(EXISTS "${GFLAGS_STATIC_LIB}")
    list(APPEND _wa_libs "$<TARGET_FILE:facebook::gflags_static>")
  endif()
  target_link_options(velox PRIVATE
    "-Wl,--whole-archive" ${_wa_libs} "-Wl,--no-whole-archive")
endif()
```

**Files changed:** `cpp/velox/CMakeLists.txt`

---

### Fix 10 — Boost RTTI symbols undefined at runtime (cannot whole-archive)

**Error (runtime — JVM startup, after Fix 9):**
```
java.lang.UnsatisfiedLinkError: .../libvelox.so:
  undefined symbol: _ZTIN5boost15program_options22error_with_option_nameE
```

**Root cause:** Boost `.a` files on Ubuntu 24.04 x86_64 are **not compiled
with `-fPIC`**. Attempting to `--whole-archive` them into `libvelox.so` causes
a build-time error:

```
relocation R_X86_64_PC32 against symbol in libboost_program_options.a
  cannot be used in a shared object; recompile with -fPIC
```

The only option is to link the Boost **shared libraries** (`.so`) as
`DT_NEEDED` entries in `libvelox.so`, so the dynamic linker loads them
automatically when `libvelox.so` is loaded.

**Fix:** Add a `foreach` loop in `cpp/velox/CMakeLists.txt` that `find_library`s
each required system `.so` and adds it via `target_link_libraries`. The
`find_library` cache variable must be `unset(... CACHE)` each iteration so
cmake re-searches for the next library name:

```cmake
set(_dt_needed_libs
    boost_filesystem boost_program_options boost_context
    snappy lz4 zstd z lzma bz2 ssl crypto fmt double-conversion event sodium)

foreach(_syslib IN LISTS _dt_needed_libs)
  find_library(_syslib_path NAMES ${_syslib}
    PATHS /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu
          /usr/lib/aarch64-linux-gnu /lib/aarch64-linux-gnu
          /usr/local/lib /usr/lib /lib
    NO_DEFAULT_PATH)
  if(_syslib_path)
    message(STATUS "Adding DT_NEEDED: ${_syslib_path}")
    target_link_libraries(velox PUBLIC ${_syslib_path})
  endif()
  unset(_syslib_path CACHE)   # MUST unset or cmake returns the cached first result for all iterations
endforeach()
```

**Note on lz4/zstd:** Arrow bundles its own lz4 and zstd
(`ARROW_DEPENDENCY_SOURCE=BUNDLED`) — those symbols are already in
`libgluten.so` / `libvelox.so`. The `lz4` and `zstd` entries in the list above
are harmless; they add `DT_NEEDED` entries for system lz4/zstd but the
Arrow-bundled symbols take precedence at runtime.

**Files changed:** `cpp/velox/CMakeLists.txt`

---

### Fix 11 — OpenSSL / snappy / lzma undefined symbols

**Error (runtime — after adding Boost DT_NEEDED):**
```
java.lang.UnsatisfiedLinkError: .../libvelox.so:
  undefined symbol: X509_NAME_free      (OpenSSL)
  undefined symbol: _ZTIN6snappy6SourceE (snappy)
  undefined symbol: lzma_easy_encoder    (lzma)
```

**Root cause:** The initial DT_NEEDED list was incomplete — `ssl`, `crypto`,
`snappy`, and `lzma` were not included. The `find_library` search paths also
did not cover all Ubuntu 24.04 multiarch paths.

**Fix:** Add `snappy`, `ssl`, `crypto`, `lzma` to the `_dt_needed_libs` list
in Fix 10, and broaden the `PATHS` to include both `/usr/lib/x86_64-linux-gnu`
and `/lib/x86_64-linux-gnu` (and the arm64 equivalents). Final list:

```
boost_filesystem boost_program_options boost_context snappy
lz4 zstd z lzma bz2 ssl crypto fmt double-conversion event sodium
```

**Files changed:** `cpp/velox/CMakeLists.txt`

---

### Fix 12 — Geo function registration crash when `VELOX_ENABLE_GEO=OFF`

**Error (runtime — JVM startup):**
```
java.lang.UnsatisfiedLinkError: .../libvelox.so:
  undefined symbol: _ZN8facebook5velox9functions25registerGeometryFunctionsERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE
  undefined symbol: _ZN8facebook5velox9functions34registerSphericalGeographyFunctionsEv
  undefined symbol: _ZN8facebook5velox9functions14registerS2FunctionsERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE
```

**Root cause:** `cpp/velox/operators/functions/RegistrationAllFunctions.cc`
called `registerGeometryFunctions`, `registerSphericalGeographyFunctions`, and
`registerS2Functions` unconditionally. When Velox is built with
`VELOX_ENABLE_GEO=OFF` (the default on Ubuntu 24.04 — see Fix 6), those
functions are not compiled into `libvelox.a`, so `libvelox.so` has them as
undefined symbols.

**Fix:** Guard all geo function calls with `#ifdef VELOX_ENABLE_GEO` in
`RegistrationAllFunctions.cc`, and emit `target_compile_definitions(velox
PRIVATE VELOX_ENABLE_GEO)` from CMake only when GEOS is actually available:

```cpp
// RegistrationAllFunctions.cc
#ifdef VELOX_ENABLE_GEO
namespace facebook::velox::functions {
extern void registerGeometryFunctions(const std::string& prefix);
extern void registerSphericalGeographyFunctions();
extern void registerS2Functions(const std::string& prefix);
} // namespace facebook::velox::functions
#endif

// In registerAllFunctions():
#ifdef VELOX_ENABLE_GEO
  velox::functions::registerGeometryFunctions("");
  velox::functions::registerSphericalGeographyFunctions();
  velox::functions::prestosql::registerBingTileFunctions("");
  velox::functions::registerS2Functions("");
#endif
```

```cmake
# cpp/velox/CMakeLists.txt — GEOS section
find_package(geos QUIET)
if(geos_FOUND AND TARGET GEOS::geos)
  target_link_libraries(velox PUBLIC GEOS::geos)
  target_compile_definitions(velox PRIVATE VELOX_ENABLE_GEO)
elseif(EXISTS "${VELOX_BUILD_PATH}/lib/libgeos.a")
  import_library(external::geos "${VELOX_BUILD_PATH}/lib/libgeos.a")
  target_link_libraries(velox PUBLIC external::geos)
  target_compile_definitions(velox PRIVATE VELOX_ENABLE_GEO)
else()
  message(STATUS "GEOS not found — geo functions excluded (VELOX_ENABLE_GEO=OFF)")
endif()
```

**Files changed:** `cpp/velox/operators/functions/RegistrationAllFunctions.cc`,
`cpp/velox/CMakeLists.txt`

---

## Runtime Dependencies (Fresh Ubuntu 24.04 Node)

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

Note: `libre2-10` is easy to miss — it is not pulled in by any other Gluten
dependency but is required by Velox's regex functions. Missing it causes a
`libvelox.so: cannot open shared object file` error at JVM startup on a fresh
node.

---

## Auditing the Old (Broken) JAR — Actual Findings

### Audit commands

```bash
cd /tmp && rm -rf gx-audit && mkdir gx-audit && cd gx-audit
jar -xf /path/to/old-gluten-velox-bundle-spark3.5_2.12-linux_amd64-*.jar linux/amd64/

# NOTE: nm -D prints undefined symbols with a blank address field, so the
# type 'U' appears as the first whitespace-separated token — use 'grep " U "'
# not awk '$2 == "U"' (the latter matches nothing for undefined symbols).

# 1. All undefined symbols that are NOT from standard glibc/libstdc++/libgcc
nm -D linux/amd64/libvelox.so | grep ' U ' \
  | grep -vE '@(GLIBC_|CXXABI_|GCC_|GLIBCXX_)' \
  | sort

# 2. Specifically: undefined RTTI typeinfo (_ZTI) and vtables (_ZTV)
nm -D linux/amd64/libvelox.so | grep ' U ' | grep -E '_ZTI|_ZTV'

# 3. Folly-specific undefined symbols
nm -D linux/amd64/libvelox.so | grep ' U ' | grep 'folly'

# 4. Compare libgluten.so
nm -D linux/amd64/libgluten.so | grep ' U ' \
  | grep -vE '@(GLIBC_|CXXABI_|GCC_|GLIBCXX_)' \
  | sort
```

### What the audit of the old JAR showed

Command 1 returns a large list (~400+ lines). The symbols split into two
categories:

**Legitimately undefined — resolved at runtime from other shared libraries:**

| Symbol prefix | Source at runtime | Notes |
|---|---|---|
| `deflate`, `inflate`, `deflateBound` | `libz.so` (system) | zlib compression |
| `EVP_*`, `HMAC_*`, `RAND_bytes` | `libssl.so.3` / `libcrypto.so.3` (system) | OpenSSL |
| `LZ4_*`, `LZ4F_*` | `liblz4.so` (system) | LZ4 compression |
| `snappy::*` | `libsnappy.so` (system) | Snappy compression |
| `re2::*` | `libre2.so` (system) | RE2 regex |
| `fmt::*` | `libfmt.so` (system) | fmtlib formatting |
| `arrow::*` | `libgluten.so` | Arrow is statically linked into libgluten.so |
| `gluten::*` | `libgluten.so` | Gluten bridge symbols |
| `google::protobuf::*` | `libgluten.so` | Protobuf statically linked into libgluten.so |
| `google::LogMessage*` etc. | `libgluten.so` | glog statically linked into libgluten.so |

**Broken — undefined and no library provides them at runtime:**

All `folly::*` symbols in the undefined list. These were dropped from
`libvelox.a` by the linker's dead-code elimination because Gluten's bridge
code did not directly reference them. At runtime nothing provides them —
neither `libgluten.so` (which has no Folly code) nor any system library.

The number of broken Folly symbols in the old JAR is **100+**, spanning:
- `folly::futures::detail::CoreBase` and all its methods (first crash — data/RTTI)
- `folly::SharedMutex` internals
- `folly::ThreadLocal` internals
- `folly::IOThreadPoolExecutor`, `folly::CPUThreadPoolExecutor`
- `folly::fibers::Baton`, `folly::fibers::Fiber`
- `folly::IOBuf`, `folly::SingletonVault`, `folly::EventBaseManager`
- And many more Folly internals

**Why CoreBase crashed first:** `_ZTIN5folly7futures6detail8CoreBaseE` is a
RTTI typeinfo *data* symbol (not a function). The dynamic linker resolves data
symbols **eagerly** even under `RTLD_LAZY` (the default). All the other
undefined Folly *function* symbols would have failed on first call when those
code paths were exercised during query execution.

### Conclusion

The old JAR has exactly **one root cause** (missing `--whole-archive` on
`libvelox.a`) with hundreds of symptoms. The `--whole-archive` fix (Fix 8)
resolves all of them in a single rebuild — `libvelox.so` will then contain
every symbol from `libvelox.a` and no further Folly undefined-symbol crashes
are expected.

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
