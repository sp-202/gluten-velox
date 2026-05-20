/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// =============================================================================
// GeospatialUDF.cc — Velox-backed geospatial functions for Spark SQL
// =============================================================================
//
// GEOMETRY STORAGE FORMAT: Standard WKB (Well-Known Binary)
//
// Why WKB and not Velox's internal format?
//   Apache Sedona stores its GeometryUDT as JTS WKB.  In Spark's type system,
//   GeometryUDT.sqlType == BinaryType, so Gluten passes those bytes straight
//   through to Velox as varbinary.  By also using WKB here, the same Velox
//   functions work for BOTH Sedona geometry columns AND standalone usage.
//
// Standalone usage (no Sedona):
//   SELECT st_geometryfromtext(wkt_col) AS geom FROM raw_data        -- WKT→WKB
//   SELECT st_contains(boundary, geom), st_distance(a, b) FROM ...   -- WKB ops
//
// With Apache Sedona:
//   Sedona's ST_Contains(geom1, geom2) → Gluten intercepts → Velox
//   st_contains(geom1_wkb, geom2_wkb)  (GeometryUDT bytes ARE WKB)
//   (requires Scala expression mapping in VeloxSparkPlanExecApi.scala)
//
// Load this .so in Spark:
//   --conf spark.gluten.sql.columnar.backend.velox.udfLibraryPaths=libgeospatialudf.so
// =============================================================================

#include <sstream>
#include <string>

#include <velox/expression/VectorFunction.h>
#include <velox/functions/Macros.h>
#include <velox/functions/Registerer.h>
#include "udf/Udf.h"

#include <geos/geom/Coordinate.h>
#include <geos/geom/GeometryFactory.h>
#include <geos/geom/GeometryTypeId.h>
#include <geos/io/WKBReader.h>
#include <geos/io/WKBWriter.h>
#include <geos/io/WKTReader.h>
#include <geos/io/WKTWriter.h>

using namespace facebook::velox;
using namespace facebook::velox::exec;

namespace {

// Thread-local geometry factory — consistent with how Velox creates geometries.
inline const geos::geom::GeometryFactory* geomFactory() {
  thread_local auto factory = geos::geom::GeometryFactory::create();
  return factory.get();
}

// Parse WKB bytes from a StringView into a GEOS geometry.
inline std::unique_ptr<geos::geom::Geometry> parseWKB(
    const StringView& bytes) {
  std::istringstream iss(std::string(bytes.data(), bytes.size()));
  geos::io::WKBReader reader(*geomFactory());
  return reader.read(iss);
}

// Serialize a GEOS geometry to WKB, writing into a Velox output string writer.
template <typename StringWriter>
inline void writeWKB(
    const geos::geom::Geometry& geom,
    StringWriter& out) {
  std::ostringstream oss;
  geos::io::WKBWriter writer;
  writer.write(geom, oss);
  const auto& s = oss.str();
  out.append(std::string_view(s));
}

// ─── Geometry constructor / converter functions ───────────────────────────────

// st_geometryfromtext(varchar) → varbinary[WKB]
// Parses a WKT string and returns standard WKB bytes.
template <typename T>
struct StGeomFromTextFn {
  VELOX_DEFINE_FUNCTION_TYPES(T);
  bool call(out_type<Varbinary>& result, const arg_type<Varchar>& wkt) {
    try {
      geos::io::WKTReader reader(*geomFactory());
      auto geom = reader.read(std::string(wkt.data(), wkt.size()));
      writeWKB(*geom, result);
    } catch (const std::exception& e) {
      VELOX_USER_FAIL("st_geometryfromtext: {}", e.what());
    }
    return true;
  }
};

// st_geomfrombinary(varbinary[WKB]) → varbinary[WKB]
// Validates and normalises WKB input (parse + reserialise).
// Input must already be WKB — this function is the identity on valid WKB.
template <typename T>
struct StGeomFromBinaryFn {
  VELOX_DEFINE_FUNCTION_TYPES(T);
  bool call(out_type<Varbinary>& result, const arg_type<Varbinary>& wkb) {
    try {
      auto geom = parseWKB(wkb);
      writeWKB(*geom, result);
    } catch (const std::exception& e) {
      VELOX_USER_FAIL("st_geomfrombinary: {}", e.what());
    }
    return true;
  }
};

// st_astext(varbinary[WKB]) → varchar
// Converts WKB to a WKT string.
template <typename T>
struct StAsTextFn {
  VELOX_DEFINE_FUNCTION_TYPES(T);
  bool call(out_type<Varchar>& result, const arg_type<Varbinary>& geom) {
    try {
      auto geometry = parseWKB(geom);
      geos::io::WKTWriter writer;
      writer.setTrim(true);
      auto wkt = writer.write(geometry.get());
      result.append(std::string_view(wkt));
    } catch (const std::exception& e) {
      VELOX_USER_FAIL("st_astext: {}", e.what());
    }
    return true;
  }
};

// st_asbinary(varbinary[WKB]) → varbinary[WKB]
// Passthrough — geometry is already WKB.  Validates by parsing.
template <typename T>
struct StAsBinaryFn {
  VELOX_DEFINE_FUNCTION_TYPES(T);
  bool call(out_type<Varbinary>& result, const arg_type<Varbinary>& geom) {
    // Input IS WKB — just copy through (with a parse to validate).
    result.append(std::string_view(geom.data(), geom.size()));
    return true;
  }
};

// st_point(double, double) → varbinary[WKB]
// Creates a Point geometry and returns its WKB encoding.
template <typename T>
struct StPointFn {
  VELOX_DEFINE_FUNCTION_TYPES(T);
  bool call(out_type<Varbinary>& result, const double& x, const double& y) {
    try {
      geos::geom::Coordinate coord(x, y);
      auto geom = geomFactory()->createPoint(coord);
      writeWKB(*geom, result);
    } catch (const std::exception& e) {
      VELOX_USER_FAIL("st_point: {}", e.what());
    }
    return true;
  }
};

// ─── Spatial predicates ───────────────────────────────────────────────────────
//
// All accept varbinary[WKB] — compatible with Sedona's GeometryUDT serialisation
// (Sedona's GeometryUDT.sqlType == BinaryType and it serialises as JTS WKB).

#define GEO_BINARY_PRED(StructName, GeosMethod, FuncStr)                  \
  template <typename T>                                                    \
  struct StructName {                                                      \
    VELOX_DEFINE_FUNCTION_TYPES(T);                                        \
    bool call(                                                             \
        bool& result,                                                      \
        const arg_type<Varbinary>& g1,                                     \
        const arg_type<Varbinary>& g2) {                                   \
      try {                                                                \
        auto geom1 = parseWKB(g1);                                        \
        auto geom2 = parseWKB(g2);                                        \
        result = geom1->GeosMethod(geom2.get());                           \
      } catch (const std::exception& e) {                                 \
        VELOX_USER_FAIL(FuncStr ": {}", e.what());                        \
      }                                                                    \
      return true;                                                         \
    }                                                                      \
  }

GEO_BINARY_PRED(StContainsFn,   contains,   "st_contains");
GEO_BINARY_PRED(StIntersectsFn, intersects, "st_intersects");
GEO_BINARY_PRED(StDisjointFn,   disjoint,   "st_disjoint");
GEO_BINARY_PRED(StEqualsFn,     equals,     "st_equals");
GEO_BINARY_PRED(StWithinFn,     within,     "st_within");
GEO_BINARY_PRED(StCrossesFn,    crosses,    "st_crosses");
GEO_BINARY_PRED(StOverlapsFn,   overlaps,   "st_overlaps");
GEO_BINARY_PRED(StTouchesFn,    touches,    "st_touches");

#undef GEO_BINARY_PRED

// ─── Measurement functions ────────────────────────────────────────────────────

template <typename T>
struct StDistanceFn {
  VELOX_DEFINE_FUNCTION_TYPES(T);
  bool call(
      double& result,
      const arg_type<Varbinary>& g1,
      const arg_type<Varbinary>& g2) {
    try {
      auto geom1 = parseWKB(g1);
      auto geom2 = parseWKB(g2);
      result = geom1->distance(geom2.get());
    } catch (const std::exception& e) {
      VELOX_USER_FAIL("st_distance: {}", e.what());
    }
    return true;
  }
};

template <typename T>
struct StAreaFn {
  VELOX_DEFINE_FUNCTION_TYPES(T);
  bool call(double& result, const arg_type<Varbinary>& geom) {
    try {
      result = parseWKB(geom)->getArea();
    } catch (const std::exception& e) {
      VELOX_USER_FAIL("st_area: {}", e.what());
    }
    return true;
  }
};

template <typename T>
struct StLengthFn {
  VELOX_DEFINE_FUNCTION_TYPES(T);
  bool call(double& result, const arg_type<Varbinary>& geom) {
    try {
      result = parseWKB(geom)->getLength();
    } catch (const std::exception& e) {
      VELOX_USER_FAIL("st_length: {}", e.what());
    }
    return true;
  }
};

// ─── Accessor functions ───────────────────────────────────────────────────────

template <typename T>
struct StIsEmptyFn {
  VELOX_DEFINE_FUNCTION_TYPES(T);
  bool call(bool& result, const arg_type<Varbinary>& geom) {
    try {
      result = parseWKB(geom)->isEmpty();
    } catch (const std::exception& e) {
      VELOX_USER_FAIL("st_isempty: {}", e.what());
    }
    return true;
  }
};

template <typename T>
struct StIsValidFn {
  VELOX_DEFINE_FUNCTION_TYPES(T);
  bool call(bool& result, const arg_type<Varbinary>& geom) {
    try {
      result = parseWKB(geom)->isValid();
    } catch (const std::exception& e) {
      VELOX_USER_FAIL("st_isvalid: {}", e.what());
    }
    return true;
  }
};

template <typename T>
struct StXFn {
  VELOX_DEFINE_FUNCTION_TYPES(T);
  bool call(double& result, const arg_type<Varbinary>& geom) {
    try {
      auto geometry = parseWKB(geom);
      VELOX_USER_CHECK(
          geometry->getGeometryTypeId() == geos::geom::GEOS_POINT,
          "st_x only applies to Point geometry");
      if (geometry->isEmpty()) {
        return false; // NULL
      }
      result = geometry->getCoordinate()->x;
    } catch (const geos::util::GEOSException& e) {
      VELOX_USER_FAIL("st_x: {}", e.what());
    }
    return true;
  }
};

template <typename T>
struct StYFn {
  VELOX_DEFINE_FUNCTION_TYPES(T);
  bool call(double& result, const arg_type<Varbinary>& geom) {
    try {
      auto geometry = parseWKB(geom);
      VELOX_USER_CHECK(
          geometry->getGeometryTypeId() == geos::geom::GEOS_POINT,
          "st_y only applies to Point geometry");
      if (geometry->isEmpty()) {
        return false; // NULL
      }
      result = geometry->getCoordinate()->y;
    } catch (const geos::util::GEOSException& e) {
      VELOX_USER_FAIL("st_y: {}", e.what());
    }
    return true;
  }
};

} // namespace

// =============================================================================
// Gluten UDF interface
// =============================================================================

static constexpr int kNumGeospatialUdfs = 20;

static const char* kVarchar       = "varchar";
static const char* kVarbinary     = "varbinary";
static const char* kBoolean       = "boolean";
static const char* kDouble        = "double";

static const char* kArgVarchar[]      = {kVarchar};
static const char* kArgVarbinary[]    = {kVarbinary};
static const char* kArgTwoVarbinary[] = {kVarbinary, kVarbinary};
static const char* kArgTwoDouble[]    = {kDouble, kDouble};

static gluten::UdfEntry kGeospatialUdfs[kNumGeospatialUdfs] = {
    // Constructor / converter — WKT/WKB ↔ varbinary[WKB]
    {"st_geometryfromtext", kVarbinary, 1, kArgVarchar,       false, true},
    {"st_geomfrombinary",   kVarbinary, 1, kArgVarbinary,     false, true},
    {"st_astext",           kVarchar,   1, kArgVarbinary,     false, true},
    {"st_asbinary",         kVarbinary, 1, kArgVarbinary,     false, true},
    {"st_point",            kVarbinary, 2, kArgTwoDouble,     false, true},
    // Spatial predicates — (WKB, WKB) → boolean
    {"st_contains",         kBoolean,   2, kArgTwoVarbinary,  false, true},
    {"st_intersects",       kBoolean,   2, kArgTwoVarbinary,  false, true},
    {"st_disjoint",         kBoolean,   2, kArgTwoVarbinary,  false, true},
    {"st_equals",           kBoolean,   2, kArgTwoVarbinary,  false, true},
    {"st_within",           kBoolean,   2, kArgTwoVarbinary,  false, true},
    {"st_crosses",          kBoolean,   2, kArgTwoVarbinary,  false, true},
    {"st_overlaps",         kBoolean,   2, kArgTwoVarbinary,  false, true},
    {"st_touches",          kBoolean,   2, kArgTwoVarbinary,  false, true},
    // Measurements — WKB → scalar
    {"st_distance",         kDouble,    2, kArgTwoVarbinary,  false, true},
    {"st_area",             kDouble,    1, kArgVarbinary,     false, true},
    {"st_length",           kDouble,    1, kArgVarbinary,     false, true},
    // Accessors
    {"st_isempty",          kBoolean,   1, kArgVarbinary,     false, true},
    {"st_isvalid",          kBoolean,   1, kArgVarbinary,     false, true},
    {"st_x",                kDouble,    1, kArgVarbinary,     false, true},
    {"st_y",                kDouble,    1, kArgVarbinary,     false, true},
};

DEFINE_GET_NUM_UDF {
  return kNumGeospatialUdfs;
}

DEFINE_GET_UDF_ENTRIES {
  for (int i = 0; i < kNumGeospatialUdfs; i++) {
    udfEntries[i] = kGeospatialUdfs[i];
  }
}

DEFINE_REGISTER_UDF {
  // Constructor / converter
  facebook::velox::registerFunction<StGeomFromTextFn, Varbinary, Varchar>(
      {"st_geometryfromtext"});
  facebook::velox::registerFunction<StGeomFromBinaryFn, Varbinary, Varbinary>(
      {"st_geomfrombinary"});
  facebook::velox::registerFunction<StAsTextFn, Varchar, Varbinary>(
      {"st_astext"});
  facebook::velox::registerFunction<StAsBinaryFn, Varbinary, Varbinary>(
      {"st_asbinary"});
  facebook::velox::registerFunction<StPointFn, Varbinary, double, double>(
      {"st_point"});
  // Spatial predicates
  facebook::velox::registerFunction<StContainsFn, bool, Varbinary, Varbinary>(
      {"st_contains"});
  facebook::velox::registerFunction<StIntersectsFn, bool, Varbinary, Varbinary>(
      {"st_intersects"});
  facebook::velox::registerFunction<StDisjointFn, bool, Varbinary, Varbinary>(
      {"st_disjoint"});
  facebook::velox::registerFunction<StEqualsFn, bool, Varbinary, Varbinary>(
      {"st_equals"});
  facebook::velox::registerFunction<StWithinFn, bool, Varbinary, Varbinary>(
      {"st_within"});
  facebook::velox::registerFunction<StCrossesFn, bool, Varbinary, Varbinary>(
      {"st_crosses"});
  facebook::velox::registerFunction<StOverlapsFn, bool, Varbinary, Varbinary>(
      {"st_overlaps"});
  facebook::velox::registerFunction<StTouchesFn, bool, Varbinary, Varbinary>(
      {"st_touches"});
  // Measurements
  facebook::velox::registerFunction<StDistanceFn, double, Varbinary, Varbinary>(
      {"st_distance"});
  facebook::velox::registerFunction<StAreaFn, double, Varbinary>(
      {"st_area"});
  facebook::velox::registerFunction<StLengthFn, double, Varbinary>(
      {"st_length"});
  // Accessors
  facebook::velox::registerFunction<StIsEmptyFn, bool, Varbinary>(
      {"st_isempty"});
  facebook::velox::registerFunction<StIsValidFn, bool, Varbinary>(
      {"st_isvalid"});
  facebook::velox::registerFunction<StXFn, double, Varbinary>(
      {"st_x"});
  facebook::velox::registerFunction<StYFn, double, Varbinary>(
      {"st_y"});
}
