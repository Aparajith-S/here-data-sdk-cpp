# Copyright (C) 2019-2020 HERE Europe B.V.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0
# License-Filename: LICENSE

# Do not export external projects to the cmake user package registry
set(CMAKE_EXPORT_NO_PACKAGE_REGISTRY ON)

include(ProcessorCount)
ProcessorCount(N)

# get CMAKE_MAKE_PROGRAM to know when to change build flags for Windows command line build
get_filename_component(compilername ${CMAKE_MAKE_PROGRAM} NAME)

if(NOT N EQUAL 0)
    if (${compilername} STREQUAL "MSBuild.exe")
        set(EXTERNAL_BUILD_FLAGS "-- /m:${N}")
    elseif(NOT (CMAKE_GENERATOR STREQUAL Xcode))
        set(EXTERNAL_BUILD_FLAGS "-- -j${N}")
    endif()
endif()

set(COMMON_PLATFORM_FLAGS)

if(ANDROID)
  set(COMMON_PLATFORM_FLAGS -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE} 
                            -DANDROID_PLATFORM=${ANDROID_PLATFORM}
                            -DANDROID_STL=${ANDROID_STL}
                            -DANDROID_ABI=${ANDROID_ABI})
elseif(IOS)
  set(COMMON_PLATFORM_FLAGS -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
                            -DPLATFORM=${PLATFORM}
                            -DSIMULATOR=${SIMULATOR})
endif()

# Setup versions for external project 
set(OLP_SDK_CPP_GOOGLETEST_URL "https://github.com/google/googletest.git")
set(OLP_SDK_CPP_GOOGLETEST_TAG "release-1.10.0")

set(OLP_SDK_CPP_SNAPPY_URL "https://github.com/google/snappy.git")
set(OLP_SDK_CPP_SNAPPY_TAG "1.1.7")

set(OLP_SDK_CPP_LEVELDB_URL "https://github.com/google/leveldb.git")
set(OLP_SDK_CPP_LEVELDB_TAG "1.21")

set(OLP_SDK_CPP_RAPIDJSON_URL "https://github.com/Tencent/rapidjson.git")
set(OLP_SDK_CPP_RAPIDJSON_TAG "master")

set(OLP_SDK_CPP_BOOST_URL "https://github.com/boostorg/boost.git")
set(OLP_SDK_CPP_BOOST_TAG "boost-1.72.0")

# Add external projects
find_package(GTest QUIET)
if(NOT TARGET GTest AND NOT GTest_FOUND)
    add_subdirectory(googletest)
endif()

find_package(RapidJSON 1.1.0 QUIET)
if(NOT TARGET RapidJSON AND NOT RapidJSON_FOUND)
    add_subdirectory(rapidjson)
endif()

find_package(leveldb QUIET)
if(NOT TARGET leveldb AND NOT leveldb_FOUND)
    add_subdirectory(leveldb)
    set(leveldb_DIR ${EXTERNAL_leveldb_DIR} PARENT_SCOPE)
    set(leveldb_INCLUDE_DIR ${EXTERNAL_leveldb_INCLUDE_DIR} PARENT_SCOPE)
    set(Snappy_DIR ${EXTERNAL_Snappy_DIR} PARENT_SCOPE)
endif()

find_package(Boost QUIET)
if(NOT TARGET Boost AND NOT Boost_FOUND)
    add_subdirectory(boost)
    set(BOOST_ROOT ${EXTERNAL_BOOST_ROOT} PARENT_SCOPE)
    set(Boost_INCLUDE_DIR ${EXTERNAL_BOOST_ROOT_INCLUDE} PARENT_SCOPE)
    set(Boost_LIBRARY_DIR ${EXTERNAL_BOOST_ROOT_LIB} PARENT_SCOPE)
endif()
