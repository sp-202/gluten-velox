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
// Geometry is passed as Spark BinaryType (varbinary), encoded in Velox's
// internal geometry format (GeometrySerializer/GeometryDeserializer).
//
// Usage in Spark SQL:
//   -- Parse once:
//   SELECT st_geometryfromtext(wkt_col) AS geom FROM raw_data
//   -- Query:
//   SELECT st_contains(boundary, geom), st_distance(a, b) FROM spatial_data
//
// Load this .so in Spark via:
//   --conf spark.gluten.sql.columnar.backend.velox.udfLibraryPaths=libgeospatialudf.so
// =============================================================================

#include <sstream>
#include <string>

#include <velox/expression/VectorFunction.h>
#include <velox/functions/Macros.h>
#include <velox/functions/Registerer.h>
#include "udf/Udf.h"

#include "velox/common/geospatial/GeometrySerde.h"
#include "velox/functions/prestosql/geospatial/GeometryUtils.h"

#include <geos/geom/Coordinate.h>
#include <geos/geom/GeometryFactory.h>
#include <geos/io/WKBReader.h>
#include <geos/io/WKBWriter.h>
#include <geos/io/WKTReader.h>
#include <geos/io/WKTWriter.h>

using namespace facebook::velox;
using namespace facebook::velox::exec;
using facebook::velox::common::geospatial::GeometryDeserializer;
using facebook::velox::common::geospatial::GeometrySerializer;

namespace {

// ─── Geometry constructor / converter functions ───────────────────────────────

// st_geometryfromtext(varchar) → varbinary
// Parses WKT and returns Velox's internal geometry binary format.
template <typename T>
struct StGeomFromTextFn {
  VELOX_DEFINE_FUNCTION_TYPES(T);
  bool call(out_type<Varbinary>& result, const arg_type<Varchar>& wkt) {
    try {
      geos::io::WKTReader reader;
      auto geom = reader.read(std::string(wkt.data(), wkt.size()));
      GeometrySerializer::serialize(*geom, result);
    } catch (const std::exception& e) {
      VELOX_USER_FAIL("st_geometryfromtext: {}", e.what());
    }
    return true;
  }
};

// st_geomfrombinary(varbinary) → varbinary
// Converts standard WKB bytes to Velox's internal geometry binary format.
template <typename T>
struct StGeomFromBinaryFn {
  VELOX_DEFINE_FUNCTION_TYPES(T);
  bool call(out_type<Varbinary>& result, const arg_type<Varbinary>& wkb) {
    try {
      std::istringstream iss(std::string(wkb.data(), wkb.size()));
      geos::io::WKBReader reader;
      auto geom = reader.read(iss);
      GeometrySerializer::serialize(*geom, result);
    } catch (const std::exception& e) {
      VELOX_USER_FAIL("st_geomfrombinary: {}", e.what());
    }
    return true;
  }
};

// st_astext(varbinary) → varchar
// Converts Velox's geometry binary to WKT string.
template <typename T>
struct StAsTextFn {
  VELOX_DEFINE_FUNCTION_TYPES(T);
  bool call(out_type<Varchar>& result, const arg_type<Varbinary>& geom) {
    try {
      auto geometry = GeometryDeserializer::deserialize(geom);
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

// st_asbinary(varbinary) → varbinary
// Converts Velox's geometry binary to standard WKB format.
template <typename T>
struct StAsBinaryFn {
  VELOX_DEFINE_FUNCTION_TYPES(T);
  bool call(out_type<Varbinary>& result, const arg_type<Varbinary>& geom) {
    try {
      auto geometry = GeometryDeserializer::deserialize(geom);
      std::ostringstream oss;
      geos::io::WKBWriter writer;
      writer.write(*geometry, oss);
      auto bytes = oss.str();
      result.append(std::string_view(bytes));
    } catch (const std::exception& e) {
      VELOX_USER_FAIL("st_asbinary: {}", e.what());
    }
    return true;
  }
};

// st_point(double, double) → varbinary
// Creates a Point geometry from x, y coordinates.
template <typename T>
struct StPointFn {
  VELOX_DEFINE_FUNCTION_TYPES(T);
  bool call(out_type<Varbinary>& result, const double& x, const double& y) {
    try {
      auto* factory = GeometryDeserializer::getGeometryFactory();
      geos::geom::Coordinate coord(x, y);
      auto geom = factory->createPoint(coord);
      GeometrySerializer::serialize(*geom, result);
    } catch (const std::exception& e) {
      VELOX_USER_FAIL("st_point: {}", e.what());
    }
    return true;
  }
};

// ─── Spatial predicates ───────────────────────────────────────────────────────

// Macro to define the 8 binary spatial predicates that follow the same pattern:
// (varbinary, varbinary) → boolean  using a GEOS geometry method.
#define GEO_BINARY_PRED(StructName, GeosMethod, FuncStr)                  \
  template <typename T>                                                    \
  struct StructName {                                                      \
    VELOX_DEFINE_FUNCTION_TYPES(T);                                        \
    bool call(                                                             \
        bool& result,                                                      \
        const arg_type<Varbinary>& g1,                                     \
        const arg_type<Varbinary>& g2) {                                   \
      try {                                                                \
        auto geom1 = GeometryDeserializer::deserialize(g1);               \
        auto geom2 = GeometryDeserializer::deserialize(g2);               \
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

// st_distance(varbinary, varbinary) → double
template <typename T>
struct StDistanceFn {
  VELOX_DEFINE_FUNCTION_TYPES(T);
  bool call(
      double& result,
      const arg_type<Varbinary>& g1,
      const arg_type<Varbinary>& g2) {
    try {
      auto geom1 = GeometryDeserializer::deserialize(g1);
      auto geom2 = GeometryDeserializer::deserialize(g2);
      result = geom1->distance(geom2.get());
    } catch (const std::exception& e) {
      VELOX_USER_FAIL("st_distance: {}", e.what());
    }
    return true;
  }
};

// st_area(varbinary) → double
template <typename T>
struct StAreaFn {
  VELOX_DEFINE_FUNCTION_TYPES(T);
  bool call(double& result, const arg_type<Varbinary>& geom) {
    auto geometry = GeometryDeserializer::deserialize(geom);
    result = geometry->getArea();
    return true;
  }
};

// st_length(varbinary) → double
template <typename T>
struct StLengthFn {
  VELOX_DEFINE_FUNCTION_TYPES(T);
  bool call(double& result, const arg_type<Varbinary>& geom) {
    auto geometry = GeometryDeserializer::deserialize(geom);
    result = geometry->getLength();
    return true;
  }
};

// ─── Accessor functions ───────────────────────────────────────────────────────

// st_isempty(varbinary) → boolean
template <typename T>
struct StIsEmptyFn {
  VELOX_DEFINE_FUNCTION_TYPES(T);
  bool call(bool& result, const arg_type<Varbinary>& geom) {
    auto geometry = GeometryDeserializer::deserialize(geom);
    result = geometry->isEmpty();
    return true;
  }
};

// st_isvalid(varbinary) → boolean
template <typename T>
struct StIsValidFn {
  VELOX_DEFINE_FUNCTION_TYPES(T);
  bool call(bool& result, const arg_type<Varbinary>& geom) {
    auto geometry = GeometryDeserializer::deserialize(geom);
    result = geometry->isValid();
    return true;
  }
};

// st_x(varbinary) → double   (x coordinate of a Point; NULL if empty)
template <typename T>
struct StXFn {
  VELOX_DEFINE_FUNCTION_TYPES(T);
  bool call(double& result, const arg_type<Varbinary>& geom) {
    auto geometry = GeometryDeserializer::deserialize(geom);
    VELOX_USER_CHECK(
        geometry->getGeometryTypeId() == geos::geom::GEOS_POINT,
        "st_x only applies to Point geometry");
    if (geometry->isEmpty()) {
      return false; // NULL
    }
    result = geometry->getCoordinate()->x;
    return true;
  }
};

// st_y(varbinary) → double   (y coordinate of a Point; NULL if empty)
template <typename T>
struct StYFn {
  VELOX_DEFINE_FUNCTION_TYPES(T);
  bool call(double& result, const arg_type<Varbinary>& geom) {
    auto geometry = GeometryDeserializer::deserialize(geom);
    VELOX_USER_CHECK(
        geometry->getGeometryTypeId() == geos::geom::GEOS_POINT,
        "st_y only applies to Point geometry");
    if (geometry->isEmpty()) {
      return false; // NULL
    }
    result = geometry->getCoordinate()->y;
    return true;
  }
};

} // namespace

// =============================================================================
// Gluten UDF interface — registry entries and registration
// =============================================================================

static constexpr int kNumGeospatialUdfs = 20;

static const char* kVarchar   = "varchar";
static const char* kVarbinary = "varbinary";
static const char* kBoolean   = "boolean";
static const char* kDouble    = "double";

static const char* kArgVarchar[]          = {kVarchar};
static const char* kArgVarbinary[]        = {kVarbinary};
static const char* kArgTwoVarbinary[]     = {kVarbinary, kVarbinary};
static const char* kArgTwoDouble[]        = {kDouble, kDouble};

static gluten::UdfEntry kGeospatialUdfs[kNumGeospatialUdfs] = {
    // Constructor / converter
    {"st_geometryfromtext", kVarbinary, 1, kArgVarchar,       false, true},
    {"st_geomfrombinary",   kVarbinary, 1, kArgVarbinary,     false, true},
    {"st_astext",           kVarchar,   1, kArgVarbinary,     false, true},
    {"st_asbinary",         kVarbinary, 1, kArgVarbinary,     false, true},
    {"st_point",            kVarbinary, 2, kArgTwoDouble,     false, true},
    // Spatial predicates
    {"st_contains",         kBoolean,   2, kArgTwoVarbinary,  false, true},
    {"st_intersects",       kBoolean,   2, kArgTwoVarbinary,  false, true},
    {"st_disjoint",         kBoolean,   2, kArgTwoVarbinary,  false, true},
    {"st_equals",           kBoolean,   2, kArgTwoVarbinary,  false, true},
    {"st_within",           kBoolean,   2, kArgTwoVarbinary,  false, true},
    {"st_crosses",          kBoolean,   2, kArgTwoVarbinary,  false, true},
    {"st_overlaps",         kBoolean,   2, kArgTwoVarbinary,  false, true},
    {"st_touches",          kBoolean,   2, kArgTwoVarbinary,  false, true},
    // Measurements
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
