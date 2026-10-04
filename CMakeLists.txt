cmake_minimum_required(VERSION 3.26)
project(snappy_turns CXX)

# Dusklight version to build against: a release tag (e.g. "v1.5.0") or a commit SHA.
# Override with -DDUSKLIGHT_VERSION=<tag-or-sha> to match the Dusklight build you run.
if (NOT DUSKLIGHT_VERSION)
    set(DUSKLIGHT_VERSION "e916b61ca00c391a1dd73ce4f80ed87ae3a12227")
endif ()

# Fetches the Dusklight sources into ./dusklight and provides DUSKLIGHT_DIR.
# Use -DDUSKLIGHT_DIR=<path> to point at an existing Dusklight checkout instead.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchDusklight.cmake")
add_subdirectory("${DUSKLIGHT_DIR}/sdk" dusklight-sdk EXCLUDE_FROM_ALL)

add_mod(snappy_turns
        FEATURES game
        SOURCES src/mod.cpp
        MOD_JSON mod.json
)
