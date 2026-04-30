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
