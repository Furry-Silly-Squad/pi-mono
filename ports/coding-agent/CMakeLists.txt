cmake_minimum_required(VERSION 3.20)
project(coding_agent_port VERSION 0.1.0 LANGUAGES CXX)

include(FetchContent)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CODING_AGENT_PORT_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-Wall -Wextra -Wpedantic)
  if(CODING_AGENT_PORT_WARNINGS_AS_ERRORS)
    add_compile_options(-Werror)
  endif()
endif()

find_package(CURL REQUIRED)
find_package(Readline QUIET)

if(NOT TARGET Readline::Readline)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(PC_READLINE QUIET readline)
  endif()

  find_path(READLINE_INCLUDE_DIR
    NAMES readline/readline.h
    HINTS
      ${PC_READLINE_INCLUDE_DIRS}
      /opt/homebrew/opt/readline/include
      /usr/local/opt/readline/include
      /usr/include
  )

  find_library(READLINE_LIBRARY
    NAMES readline
    HINTS
      ${PC_READLINE_LIBRARY_DIRS}
      /opt/homebrew/opt/readline/lib
      /usr/local/opt/readline/lib
      /usr/lib
  )

  if(READLINE_INCLUDE_DIR AND READLINE_LIBRARY)
    add_library(Readline::Readline UNKNOWN IMPORTED)
    set_target_properties(Readline::Readline PROPERTIES
      IMPORTED_LOCATION "${READLINE_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${READLINE_INCLUDE_DIR}"
    )
  else()
    message(FATAL_ERROR "Readline not found. Install readline (e.g. 'brew install readline') and ensure pkg-config can find it.")
  endif()
endif()

FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(nlohmann_json)

file(GLOB_RECURSE CODING_AGENT_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_executable(coding-agent ${CODING_AGENT_SOURCES})

target_include_directories(coding-agent
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(coding-agent PRIVATE CURL::libcurl Readline::Readline nlohmann_json::nlohmann_json)

# Deterministic local checks (no provider): pure logic + fixture JSONL
add_executable(coding-agent-fileops-test test/file_ops_self_test.cpp src/file_ops.cpp)
target_include_directories(coding-agent-fileops-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(coding-agent-fileops-test PRIVATE nlohmann_json::nlohmann_json)

add_executable(coding-agent-branch-summary-test test/branch_summary_self_test.cpp src/branch_summary.cpp src/session.cpp src/compaction.cpp src/file_ops.cpp)
target_include_directories(coding-agent-branch-summary-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(coding-agent-branch-summary-test PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(
  coding-agent-branch-summary-test PRIVATE
  "CODING_AGENT_TEST_FIXTURE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures\""
)

add_executable(coding-agent-session-store-test test/session_store_roundtrip_test.cpp src/session.cpp src/file_ops.cpp)
target_include_directories(coding-agent-session-store-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(coding-agent-session-store-test PRIVATE nlohmann_json::nlohmann_json)

add_executable(coding-agent-branch-traversal-test test/branch_traversal_test.cpp src/session.cpp src/branch_summary.cpp src/compaction.cpp src/file_ops.cpp)
target_include_directories(coding-agent-branch-traversal-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(coding-agent-branch-traversal-test PRIVATE nlohmann_json::nlohmann_json)

add_executable(coding-agent-compaction-carry-forward-test test/compaction_carry_forward_test.cpp src/compaction.cpp src/file_ops.cpp)
target_include_directories(coding-agent-compaction-carry-forward-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(coding-agent-compaction-carry-forward-test PRIVATE nlohmann_json::nlohmann_json)

# AgentSession integration test (Phase 7). Pulls in the full runtime except provider/main.
add_executable(coding-agent-agent-session-test
  test/agent_session_test.cpp
  src/agent_session.cpp
  src/compaction.cpp
  src/context_loader.cpp
  src/file_ops.cpp
  src/session_entry.cpp
  src/system_prompt.cpp
  src/tools/tool_registry.cpp
  src/tools/bash_tool.cpp
  src/tools/edit_tool.cpp
  src/tools/find_tool.cpp
  src/tools/grep_tool.cpp
  src/tools/ls_tool.cpp
  src/tools/read_tool.cpp
  src/tools/write_tool.cpp
)
target_include_directories(coding-agent-agent-session-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(coding-agent-agent-session-test PRIVATE nlohmann_json::nlohmann_json)

enable_testing()
add_test(NAME coding-agent-fileops COMMAND coding-agent-fileops-test)
add_test(NAME coding-agent-branch-summary COMMAND coding-agent-branch-summary-test)
add_test(NAME coding-agent-session-store COMMAND coding-agent-session-store-test)
add_test(NAME coding-agent-branch-traversal COMMAND coding-agent-branch-traversal-test)
add_test(NAME coding-agent-compaction-carry-forward COMMAND coding-agent-compaction-carry-forward-test)
add_test(NAME coding-agent-agent-session COMMAND coding-agent-agent-session-test)
