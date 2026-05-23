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
package org.apache.gluten.spi

import org.apache.gluten.jni.JniLibLoader

class SharedLibraryLoaderUbuntu2404 extends SharedLibraryLoader {

  override def accepts(osName: String, osVersion: String): Boolean = {
    osName.contains("Ubuntu") && osVersion.startsWith("24.04")
  }

  override def loadLib(loader: JniLibLoader): Unit = {
    // Ubuntu 24.04 build links against system libs — they are NOT bundled in the JAR.
    // The dynamic linker resolves them automatically from the system at load time.
    // Only load the files actually present in the JAR: arrow JNI libs, then gluten/velox.
    // JAR paths differ by architecture:
    //   x86_64  → arrow: x86_64/,   gluten/velox: linux/amd64/
    //   aarch64 → arrow: aarch64/,  gluten/velox: linux/aarch64/
    val arch = System.getProperty("os.arch", "")
    val (arrowPrefix, glutenPrefix) = if (arch == "aarch64") {
      ("aarch64", "linux/aarch64")
    } else {
      ("x86_64", "linux/amd64")
    }
    loader.loadAndCreateLink(s"$arrowPrefix/libarrow_cdata_jni.so", "libarrow_cdata_jni.so")
    loader.loadAndCreateLink(s"$arrowPrefix/libarrow_dataset_jni.so", "libarrow_dataset_jni.so")
    loader.loadAndCreateLink(s"$glutenPrefix/libgluten.so", "libgluten.so")
    loader.loadAndCreateLink(s"$glutenPrefix/libvelox.so", "libvelox.so")
  }

}
