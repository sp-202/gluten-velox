# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.
# The ASF licenses this file to You under the Apache License, Version 2.0
# (the "License"); you may not use this file except in compliance with
# the License.  You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

add_library(myfirstudf SHARED "MyFirstUDF.cc")
target_link_libraries(myfirstudf velox)

# Geospatial UDF library: ST_* functions backed by Velox's GEOS implementation.
# Requires Velox built with VELOX_ENABLE_GEO=ON (set in build-velox.sh).
# Geometry is stored as varbinary (Velox's internal binary format).
add_library(geospatialudf SHARED "GeospatialUDF.cc")
# external::geos is defined in cpp/velox/CMakeLists.txt — either the system GEOS
# package or Velox's bundled libgeos.a. Linking it directly gives us both the
# library and (via GEOS::geos) the include dirs when a system package is found.
# For the bundled case, we add the Velox build include path explicitly.
target_link_libraries(geospatialudf velox external::geos)
# Velox installs GEOS headers to /usr/local/include/geos during its build.
# The import_library() path for bundled GEOS does not propagate include dirs,
# so we add it explicitly here.
target_include_directories(geospatialudf SYSTEM PRIVATE /usr/local/include)
