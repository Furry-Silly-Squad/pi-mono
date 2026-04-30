cmake_minimum_required(VERSION 3.20)
project(coding_agent_port VERSION 0.1.0 LANGUAGES CXX)

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

add_executable(coding-agent
  src/main.cpp
  src/agent.cpp
  src/config.cpp
  src/providers/llama_cpp_provider.cpp
)

target_include_directories(coding-agent
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(coding-agent PRIVATE CURL::libcurl)
