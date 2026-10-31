cmake_minimum_required(VERSION 3.14)
project(rkllm_console VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ----------------------------------------------------------------------------
# Options
# ----------------------------------------------------------------------------
option(RKLLM_MOCK
       "Build with a fake engine (no NPU / rkllm library needed) to preview the UI on any Linux PC"
       OFF)

set(RKLLM_ROOT "" CACHE PATH
    "Path to rknn-llm/rkllm-runtime/Linux/librkllm_api (contains include/rkllm.h and aarch64/librkllmrt.so)")

set(RKLLM_VERSION_LABEL "1.3.1" CACHE STRING "Runtime version text shown in the header banner")

find_package(Threads REQUIRED)
find_package(SQLite3)
if(NOT SQLite3_FOUND)
  message(FATAL_ERROR
    "\nSQLite3 development files not found.\n"
    "Install them with:  sudo apt install -y libsqlite3-dev\n")
endif()

# ----------------------------------------------------------------------------
# rkllm_rag : documents -> chunks -> SQLite FTS5 search -> prompt (no model needed)
# ----------------------------------------------------------------------------
add_library(rkllm_rag STATIC
    src/core/rag.cpp
    src/core/telemetry.cpp
    src/core/utf8.cpp)
target_include_directories(rkllm_rag PUBLIC src/core)
target_compile_options(rkllm_rag PRIVATE -Wall -Wextra -O2)
target_link_libraries(rkllm_rag PUBLIC SQLite::SQLite3 Threads::Threads)

# ----------------------------------------------------------------------------
# rkllm_engine : the LLM (real RKLLM runtime, or a fake one for UI previews)
# ----------------------------------------------------------------------------
if(RKLLM_MOCK)
  add_library(rkllm_engine STATIC src/core/llm_engine_mock.cpp)
else()
  add_library(rkllm_engine STATIC src/core/llm_engine_rkllm.cpp)
endif()
target_include_directories(rkllm_engine PUBLIC src/core)
target_compile_options(rkllm_engine PRIVATE -Wall -Wextra -O2)
target_link_libraries(rkllm_engine PUBLIC Threads::Threads)

set(_rpath_targets "")

if(RKLLM_MOCK)
  message(STATUS "RKLLM_MOCK=ON -> building UI preview with the fake engine")
else()
  set(_rkllm_roots
      ${RKLLM_ROOT}
      $ENV{RKLLM_ROOT}
      $ENV{HOME}/llm/rknn-llm/rkllm-runtime/Linux/librkllm_api
      ${CMAKE_SOURCE_DIR}/third_party/rkllm
      ${CMAKE_SOURCE_DIR}/../rknn-llm/rkllm-runtime/Linux/librkllm_api)

  find_path(RKLLM_INCLUDE_DIR
            NAMES rkllm.h
            HINTS ${_rkllm_roots}
            PATH_SUFFIXES include)

  find_library(RKLLM_LIBRARY
               NAMES rkllmrt
               HINTS ${_rkllm_roots}
               PATH_SUFFIXES aarch64 lib lib64 armhf)

  if(NOT RKLLM_INCLUDE_DIR OR NOT RKLLM_LIBRARY)
    message(FATAL_ERROR
      "\nCould not find the RKLLM runtime.\n"
      "  rkllm.h        : ${RKLLM_INCLUDE_DIR}\n"
      "  librkllmrt.so  : ${RKLLM_LIBRARY}\n\n"
      "Point CMake at your SDK checkout, for example:\n"
      "  cmake -DRKLLM_ROOT=$HOME/llm/rknn-llm/rkllm-runtime/Linux/librkllm_api ..\n"
      "Tip: find ~ -name rkllm.h -o -name librkllmrt.so 2>/dev/null\n"
      "Or build the UI only:  cmake -DRKLLM_MOCK=ON ..\n")
  endif()

  message(STATUS "rkllm.h       : ${RKLLM_INCLUDE_DIR}")
  message(STATUS "librkllmrt.so : ${RKLLM_LIBRARY}")

  target_include_directories(rkllm_engine PRIVATE ${RKLLM_INCLUDE_DIR})
  target_link_libraries(rkllm_engine PUBLIC ${RKLLM_LIBRARY})
  get_filename_component(RKLLM_LIB_DIR ${RKLLM_LIBRARY} DIRECTORY)
endif()

# ----------------------------------------------------------------------------
# rkllm_console : the retro terminal app
# ----------------------------------------------------------------------------
add_executable(rkllm_console
    src/console/main.cpp
    src/console/app.cpp
    src/console/terminal.cpp
    src/console/chat_log.cpp)
target_include_directories(rkllm_console PRIVATE src/console)
target_compile_options(rkllm_console PRIVATE -Wall -Wextra -O2)
target_compile_definitions(rkllm_console PRIVATE
    RKLLM_VERSION_LABEL="${RKLLM_VERSION_LABEL}"
    RAG_DEFAULT_DOCS="${CMAKE_SOURCE_DIR}/docs"
    RAG_DEFAULT_DB="${CMAKE_SOURCE_DIR}/data/index.db")
target_link_libraries(rkllm_console PRIVATE rkllm_engine rkllm_rag)

# ----------------------------------------------------------------------------
# rkllm_web : the same assistant as a small web server (open http://BOARD_IP:8080)
# ----------------------------------------------------------------------------
add_executable(rkllm_web src/web/web_server.cpp)
target_compile_options(rkllm_web PRIVATE -Wall -Wextra -O2)
target_compile_definitions(rkllm_web PRIVATE
    RAG_DEFAULT_DOCS="${CMAKE_SOURCE_DIR}/docs"
    RAG_DEFAULT_DB="${CMAKE_SOURCE_DIR}/data/index.db"
    WEB_DEFAULT_ROOT="${CMAKE_SOURCE_DIR}/web")
target_link_libraries(rkllm_web PRIVATE rkllm_engine rkllm_rag)

# ----------------------------------------------------------------------------
# rag_tool : index / search the documents from the shell (no model needed)
# ----------------------------------------------------------------------------
add_executable(rag_tool src/tools/rag_tool.cpp)
target_compile_options(rag_tool PRIVATE -Wall -Wextra -O2)
target_link_libraries(rag_tool PRIVATE rkllm_rag)

# Bake the RKLLM library directory in, so it still loads under `sudo`
# (sudo drops LD_LIBRARY_PATH).
if(NOT RKLLM_MOCK)
  set_target_properties(rkllm_console rkllm_web PROPERTIES
      BUILD_RPATH "${RKLLM_LIB_DIR}"
      INSTALL_RPATH "${RKLLM_LIB_DIR}")
endif()

install(TARGETS rkllm_console rkllm_web rag_tool RUNTIME DESTINATION bin)
