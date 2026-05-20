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
#include <iostream>
#include <vector>
#include "udf/Udf.h"
#include "udf/examples/UdfCommon.h"

using namespace facebook::velox;
using namespace facebook::velox::exec;

namespace {

static const char* kInteger = "int";

namespace mycustomudf {

// A simple scalar function that adds 1 to an integer.
template <typename T>
struct AddOneFunction {
  VELOX_DEFINE_FUNCTION_TYPES(T);

  FOLLY_ALWAYS_INLINE void call(int32_t& result, const int32_t& a) {
    result = a + 1;
  }
};

// name: org.apache.spark.sql.custom.AddOne
// signatures:
//    int -> int
// type: SimpleFunction
class AddOneRegisterer final : public gluten::UdfRegisterer {
 public:
  int getNumUdf() override {
    return 1;
  }

  void populateUdfEntries(int& index, gluten::UdfEntry* udfEntries) override {
    // Set `allowTypeConversion` for this udf.
    udfEntries[index++] = {name_.c_str(), kInteger, 1, arg_, false, true};
  }

  void registerSignatures() override {
    facebook::velox::registerFunction<AddOneFunction, int32_t, int32_t>({name_});
  }

 private:
  const std::string name_ = "org.apache.spark.sql.custom.AddOne";
  const char* arg_[1] = {kInteger};
};

} // namespace mycustomudf

std::vector<std::shared_ptr<gluten::UdfRegisterer>>& globalRegisters() {
  static std::vector<std::shared_ptr<gluten::UdfRegisterer>> registerers;
  return registerers;
}

void setupRegisterers() {
  static bool inited = false;
  if (inited) {
    return;
  }
  auto& registerers = globalRegisters();
  registerers.push_back(std::make_shared<mycustomudf::AddOneRegisterer>());
  inited = true;
}
} // namespace

DEFINE_GET_NUM_UDF {
  using std::begin;
  using std::end;
  setupRegisterers();

  int numUdf = 0;
  for (const auto& registerer : globalRegisters()) {
    numUdf += registerer->getNumUdf();
  }
  return numUdf;
}

DEFINE_GET_UDF_ENTRIES {
  using std::begin;
  using std::end;
  setupRegisterers();

  int index = 0;
  for (const auto& registerer : globalRegisters()) {
    registerer->populateUdfEntries(index, udfEntries);
  }
}

DEFINE_REGISTER_UDF {
  using std::begin;
  using std::end;
  setupRegisterers();

  for (const auto& registerer : globalRegisters()) {
    registerer->registerSignatures();
  }
}
