# Gluten-Velox Custom UDF Fast Prototyping Guide

This directory contains the custom user-defined function (UDF) project for the Gluten Velox C++ backend. Because Gluten supports **dynamic runtime loading** of C++ UDFs, you do not need to perform slow, full engine compiles or rebuild your Spark JARs when actively developing your C++ functions.

Follow this two-phase workflow for rapid C++ UDF development.

---

## 🚀 The Two-Phase Development Workflow

### Phase 1: The Initial Full Build (One-Time Setup)
Before you can fast-prototype, you must build the initial Gluten-Velox bundle and compile the complete engine structure. Run this once from the root of `gluten-velox`:

```bash
# Clean and compile everything (creates the base C++ build environment + Spark JAR)
sudo ./docker-build.sh --rebuild-image
```

Once this finishes:
1. Your base C++ build tree will be fully cached in `cpp/build/`.
2. Your deployable Spark JAR will be placed at `output/gluten-velox-bundle-spark3.5_2.12-*.jar`.

---

### Phase 2: The 2-Second Fast Prototyping Loop (Active C++ Development)
Whenever you modify your C++ code (e.g., `MyFirstUDF.cc`), **do not run `./docker-build.sh` again.** 

Instead, run this quick one-off Docker command from the project root to compile **only your custom UDF shared library**:

```bash
sudo docker run --rm \
  -v "$(pwd):/gluten" \
  -w /gluten/cpp/build \
  gluten-velox-builder:arm64 \
  ninja myfirstudf
```

#### Why is this so fast?
* `ninja` detects that Arrow, Velox, and Gluten Core have not changed.
* It only recompiles `MyFirstUDF.cc` and links it into `libmyfirstudf.so`.
* The entire process takes **1.5 to 2 seconds**.
* Your compiled shared library is instantly generated/updated on your host at:
  `~/gluten-velox/cpp/build/velox/udf/custom_udf/libmyfirstudf.so`

---

## 🛠️ End-to-End Spark Integration

To test your UDF inside Spark, you need two pieces:
1. The **C++ Shared Library** (`libmyfirstudf.so`) — compiled in Phase 2.
2. A **Java Fallback Class** packaged as a JAR — this informs Spark's SQL planner about the function.

### Step 1: Create and Compile the Java Counterpart
Write a simple Java class on the host machine to serve as Spark's entrypoint:

```java
package org.apache.spark.sql.custom;
import org.apache.hadoop.hive.ql.exec.UDF;

public final class AddOne extends UDF {
    public Integer evaluate(Integer a) {
        if (a == null) return null;
        return a + 1; // Fallback implementation
    }
}
```

Compile it and package it into a tiny JAR:
```bash
# 1. Create source folder
mkdir -p ~/udf-java/classes

# 2. Write class to file
cat << 'EOF' > ~/udf-java/AddOne.java
package org.apache.spark.sql.custom;
import org.apache.hadoop.hive.ql.exec.UDF;
public final class AddOne extends UDF {
    public Integer evaluate(Integer a) {
        if (a == null) return null;
        return a + 1;
    }
}
EOF

# 3. Compile against Hive jar
javac -d ~/udf-java/classes ~/udf-java/AddOne.java \
  -cp "$(find ~/.m2/repository/org/apache/hive/hive-exec -name "*.jar" | head -n 1)"

# 4. Create JAR
jar -cf ~/udf-java/custom-udf-spark.jar -C ~/udf-java/classes .
```

### Step 2: Launch Spark SQL / Shell
Start your Spark session. Point it to the main Gluten JAR, your Java UDF JAR, and configure the path to your dynamically compiled C++ shared library:

```bash
spark-sql \
  --jars ~/udf-java/custom-udf-spark.jar,/home/ubuntu/gluten-velox/output/gluten-velox-bundle-spark3.5_2.12-*.jar \
  --files /home/ubuntu/gluten-velox/cpp/build/velox/udf/custom_udf/libmyfirstudf.so \
  --conf spark.plugins=org.apache.gluten.GlutenPlugin \
  --conf spark.gluten.sql.columnar.backend.velox.udfLibraryPaths=libmyfirstudf.so
```

### Step 3: Run and Verify
Register the temporary function mapping to your Java class and query it:

```sql
-- Register
CREATE TEMPORARY FUNCTION add_one AS 'org.apache.spark.sql.custom.AddOne';

-- Execute (runs fully in C++ Velox Columnar!)
SELECT add_one(val) FROM VALUES (1), (2), (3) AS tbl(val);
```

To verify it is offloaded natively, run `EXPLAIN` and check that the plan contains `ProjectExecTransformer` wrapping `HiveSimpleUDF#org.apache.spark.sql.custom.AddOne` with a caret (`^`) symbol showing zero JVM fallback!

---

## 🗺️ Advanced: Direct C++ Offloading for Apache Sedona (Spatial UDFs)

If you are developing C++ spatial UDFs to offload **Apache Sedona** functions (which bypass standard Hive/Java UDF registrations and use custom Spark Catalyst Expressions):

### 1. Map Sedona Expressions in Gluten
To prevent Sedona's custom AST nodes (e.g. `ST_Distance`) from triggering JVM fallbacks, map them to Velox function names in the Scala backend layer.

Open `backends-velox/src/main/scala/org/apache/gluten/backendsapi/velox/VeloxSparkPlanExecApi.scala` and override `extraExpressionMappings`:

```scala
import org.apache.spark.sql.sedona_sql.expressions.ST_Distance

override def extraExpressionMappings: Seq[Sig] = {
  Seq(
    Sig[ST_Distance]("st_distance")
  )
}
```

### 2. Implement the C++ Signature
Inside `MyFirstUDF.cc`, register the corresponding `st_distance` C++ function:

```cpp
class ST_DistanceRegisterer final : public gluten::UdfRegisterer {
 public:
  int getNumUdf() override { return 1; }
  void populateUdfEntries(int& index, gluten::UdfEntry* udfEntries) override {
    udfEntries[index++] = {"st_distance", "double", 2, arg_types, false, true};
  }
  void registerSignatures() override {
    facebook::velox::registerFunction<ST_DistanceFunction, double, Varchar, Varchar>({"st_distance"});
  }
 private:
  const char* arg_types[2] = {"varchar", "varchar"};
};
```
Now, whenever Spark encounters `ST_Distance(geom1, geom2)` in a spatial query, Gluten will serialize it via Substrait to `st_distance` and execute it directly in the Velox C++ layer with zero-fallback, maximum-throughput speeds!
