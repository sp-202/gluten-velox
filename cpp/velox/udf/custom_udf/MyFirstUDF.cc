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

#include <velox/expression/VectorFunction.h>
#include <velox/functions/Macros.h>
#include <velox/functions/Registerer.h>
#include "udf/Udf.h"

using namespace facebook::velox;
using namespace facebook::velox::exec;

// ---------------------------------------------------------------------------
// UDF implementation
// ---------------------------------------------------------------------------

namespace {

template <typename T>
struct AddOneFunction {
  VELOX_DEFINE_FUNCTION_TYPES(T);

  FOLLY_ALWAYS_INLINE void call(int32_t& result, const int32_t& a) {
    result = a + 1;
  }
};

} // namespace

// ---------------------------------------------------------------------------
// Static UDF registry entries (no UdfRegisterer / UdfCommon.h needed)
// ---------------------------------------------------------------------------

static const char* kAddOneName = "org.apache.spark.sql.custom.AddOne";
static const char* kInteger = "int";
static const char* kAddOneArgs[] = {kInteger};

static gluten::UdfEntry addOneEntry = {
    kAddOneName,
    kInteger,
    /*numArgs=*/1,
    kAddOneArgs,
    /*variableArity=*/false,
    /*allowTypeConversion=*/true};

// ---------------------------------------------------------------------------
// Gluten UDF interface
// ---------------------------------------------------------------------------

DEFINE_GET_NUM_UDF {
  return 1;
}

DEFINE_GET_UDF_ENTRIES {
  udfEntries[0] = addOneEntry;
}

DEFINE_REGISTER_UDF {
  facebook::velox::registerFunction<AddOneFunction, int32_t, int32_t>(
      {kAddOneName});
}
