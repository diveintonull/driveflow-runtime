cmake_minimum_required(VERSION 3.22)

project(
  DriveFlowRuntime
  VERSION 0.1.0
  DESCRIPTION "A Linux C++20 runtime for simulated vehicle sensor data"
  LANGUAGES CXX)

include(CTest)
include(FetchContent)

option(DRIVEFLOW_BUILD_TESTS "Build DriveFlow tests" ON)
option(DRIVEFLOW_BUILD_FUZZERS "Build libFuzzer targets" OFF)
option(DRIVEFLOW_ENABLE_CLANG_TIDY "Run clang-tidy while compiling" OFF)
option(DRIVEFLOW_TSAN_DISABLE_ASLR "Launch TSan tests through setarch -R" OFF)
set(DRIVEFLOW_SANITIZER "none" CACHE STRING "Sanitizer: none, address, or thread")
set_property(CACHE DRIVEFLOW_SANITIZER PROPERTY STRINGS none address thread)

add_library(driveflow_project_options INTERFACE)
target_compile_features(driveflow_project_options INTERFACE cxx_std_20)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(
    driveflow_project_options
    INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)

  if(DRIVEFLOW_SANITIZER STREQUAL "address")
    target_compile_options(
      driveflow_project_options
      INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(
      driveflow_project_options
      INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  elseif(DRIVEFLOW_SANITIZER STREQUAL "thread")
    target_compile_options(
      driveflow_project_options
      INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(
      driveflow_project_options
      INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
  elseif(NOT DRIVEFLOW_SANITIZER STREQUAL "none")
    message(FATAL_ERROR "Unsupported DRIVEFLOW_SANITIZER=${DRIVEFLOW_SANITIZER}")
  endif()
endif()

if(DRIVEFLOW_ENABLE_CLANG_TIDY)
  find_program(DRIVEFLOW_CLANG_TIDY_EXECUTABLE NAMES clang-tidy REQUIRED)
  set(DRIVEFLOW_CLANG_TIDY_COMMAND "${DRIVEFLOW_CLANG_TIDY_EXECUTABLE}")
endif()

set(DRIVEFLOW_FORMAT_SOURCES
    include/driveflow/protocol/packet.hpp
    include/driveflow/protocol/sensor_payload.hpp
    include/driveflow/net/udp_socket.hpp
    include/driveflow/simulator/sensor_simulator.hpp
    include/driveflow/runtime/epoll_receiver.hpp
    include/driveflow/runtime/runtime.hpp
    include/driveflow/runtime/runtime_config.hpp
    src/protocol/packet.cpp
    src/protocol/sensor_payload.cpp
    src/protocol/wire.hpp
    src/net/udp_socket.cpp
    src/simulator/sensor_simulator.cpp
    src/runtime/epoll_receiver.cpp
    src/runtime/runtime.cpp
    src/runtime/runtime_config.cpp
    apps/runtime_main.cpp
    apps/sensor_simulator_main.cpp
    apps/udp_sender_main.cpp
    apps/udp_receiver_main.cpp
    tests/unit/protocol_packet_test.cpp
    tests/unit/runtime_config_test.cpp
    tests/unit/sensor_payload_test.cpp
    tests/unit/sensor_simulator_test.cpp
    tests/integration/epoll_receiver_test.cpp
    tests/integration/runtime_test.cpp
    tests/integration/udp_socket_test.cpp
    tests/fuzz/decode_packet_fuzz.cpp)

find_program(DRIVEFLOW_CLANG_FORMAT_EXECUTABLE NAMES clang-format)
if(DRIVEFLOW_CLANG_FORMAT_EXECUTABLE)
  add_custom_target(
    format
    COMMAND "${DRIVEFLOW_CLANG_FORMAT_EXECUTABLE}" -i ${DRIVEFLOW_FORMAT_SOURCES}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    VERBATIM)
  add_custom_target(
    format-check
    COMMAND "${DRIVEFLOW_CLANG_FORMAT_EXECUTABLE}" --dry-run --Werror
            ${DRIVEFLOW_FORMAT_SOURCES}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    VERBATIM)
else()
  message(STATUS "clang-format not found; format targets are unavailable")
endif()

add_library(driveflow_protocol
            src/protocol/packet.cpp
            src/protocol/sensor_payload.cpp)
add_library(DriveFlow::Protocol ALIAS driveflow_protocol)
target_include_directories(
  driveflow_protocol
  PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
         "$<INSTALL_INTERFACE:include>")
target_link_libraries(driveflow_protocol PUBLIC driveflow_project_options)
if(DRIVEFLOW_ENABLE_CLANG_TIDY)
  set_property(TARGET driveflow_protocol PROPERTY CXX_CLANG_TIDY
                                                    "${DRIVEFLOW_CLANG_TIDY_COMMAND}")
endif()

add_library(driveflow_net src/net/udp_socket.cpp)
add_library(DriveFlow::Net ALIAS driveflow_net)
target_include_directories(
  driveflow_net
  PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
         "$<INSTALL_INTERFACE:include>")
target_link_libraries(driveflow_net PUBLIC driveflow_project_options)
if(DRIVEFLOW_ENABLE_CLANG_TIDY)
  set_property(TARGET driveflow_net PROPERTY CXX_CLANG_TIDY
                                             "${DRIVEFLOW_CLANG_TIDY_COMMAND}")
endif()

add_library(driveflow_simulator src/simulator/sensor_simulator.cpp)
add_library(DriveFlow::Simulator ALIAS driveflow_simulator)
target_include_directories(
  driveflow_simulator
  PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
         "$<INSTALL_INTERFACE:include>")
target_link_libraries(
  driveflow_simulator
  PUBLIC DriveFlow::Net DriveFlow::Protocol driveflow_project_options)
if(DRIVEFLOW_ENABLE_CLANG_TIDY)
  set_property(TARGET driveflow_simulator PROPERTY CXX_CLANG_TIDY
                                                   "${DRIVEFLOW_CLANG_TIDY_COMMAND}")
endif()

add_library(
  driveflow_runtime
  src/runtime/epoll_receiver.cpp
  src/runtime/runtime.cpp
  src/runtime/runtime_config.cpp)
add_library(DriveFlow::Runtime ALIAS driveflow_runtime)
target_include_directories(
  driveflow_runtime
  PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
         "$<INSTALL_INTERFACE:include>")
target_link_libraries(
  driveflow_runtime
  PUBLIC DriveFlow::Net DriveFlow::Protocol driveflow_project_options)
if(DRIVEFLOW_ENABLE_CLANG_TIDY)
  set_property(TARGET driveflow_runtime PROPERTY CXX_CLANG_TIDY
                                                 "${DRIVEFLOW_CLANG_TIDY_COMMAND}")
endif()

add_executable(driveflow_runtime_app apps/runtime_main.cpp)
set_target_properties(driveflow_runtime_app PROPERTIES OUTPUT_NAME driveflow_runtime)
target_link_libraries(driveflow_runtime_app PRIVATE DriveFlow::Runtime)

add_executable(driveflow_sensor_simulator apps/sensor_simulator_main.cpp)
target_link_libraries(driveflow_sensor_simulator PRIVATE DriveFlow::Simulator)

add_executable(driveflow_udp_sender apps/udp_sender_main.cpp)
target_link_libraries(driveflow_udp_sender PRIVATE DriveFlow::Net DriveFlow::Protocol)
add_executable(driveflow_udp_receiver apps/udp_receiver_main.cpp)
target_link_libraries(driveflow_udp_receiver PRIVATE DriveFlow::Net DriveFlow::Protocol)
if(DRIVEFLOW_ENABLE_CLANG_TIDY)
  set_property(TARGET driveflow_runtime_app PROPERTY CXX_CLANG_TIDY
                                                     "${DRIVEFLOW_CLANG_TIDY_COMMAND}")
  set_property(TARGET driveflow_sensor_simulator PROPERTY CXX_CLANG_TIDY
                                                         "${DRIVEFLOW_CLANG_TIDY_COMMAND}")
  set_property(TARGET driveflow_udp_sender PROPERTY CXX_CLANG_TIDY
                                                    "${DRIVEFLOW_CLANG_TIDY_COMMAND}")
  set_property(TARGET driveflow_udp_receiver PROPERTY CXX_CLANG_TIDY
                                                      "${DRIVEFLOW_CLANG_TIDY_COMMAND}")
endif()

if(DRIVEFLOW_BUILD_TESTS)
  find_package(GTest CONFIG QUIET)
  if(NOT GTest_FOUND)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
      googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG v1.15.2
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(googletest)
  endif()

  add_executable(protocol_packet_test tests/unit/protocol_packet_test.cpp)
  target_link_libraries(protocol_packet_test PRIVATE DriveFlow::Protocol GTest::gtest_main)
  add_executable(sensor_payload_test tests/unit/sensor_payload_test.cpp)
  target_link_libraries(sensor_payload_test PRIVATE DriveFlow::Protocol GTest::gtest_main)
  add_executable(sensor_simulator_test tests/unit/sensor_simulator_test.cpp)
  target_link_libraries(sensor_simulator_test PRIVATE DriveFlow::Simulator GTest::gtest_main)
  add_executable(udp_socket_test tests/integration/udp_socket_test.cpp)
  target_link_libraries(udp_socket_test PRIVATE DriveFlow::Net DriveFlow::Protocol
                                                GTest::gtest_main)
  add_executable(epoll_receiver_test tests/integration/epoll_receiver_test.cpp)
  target_link_libraries(
    epoll_receiver_test PRIVATE DriveFlow::Runtime GTest::gtest_main)
  add_executable(runtime_config_test tests/unit/runtime_config_test.cpp)
  target_link_libraries(
    runtime_config_test PRIVATE DriveFlow::Runtime GTest::gtest_main)
  add_executable(runtime_test tests/integration/runtime_test.cpp)
  target_link_libraries(
    runtime_test PRIVATE DriveFlow::Runtime GTest::gtest_main)
  if(DRIVEFLOW_ENABLE_CLANG_TIDY)
    set_property(TARGET protocol_packet_test PROPERTY CXX_CLANG_TIDY
                                                       "${DRIVEFLOW_CLANG_TIDY_COMMAND}")
    set_property(TARGET sensor_payload_test PROPERTY CXX_CLANG_TIDY
                                                       "${DRIVEFLOW_CLANG_TIDY_COMMAND}")
    set_property(TARGET sensor_simulator_test PROPERTY CXX_CLANG_TIDY
                                                         "${DRIVEFLOW_CLANG_TIDY_COMMAND}")
    set_property(TARGET udp_socket_test PROPERTY CXX_CLANG_TIDY
                                                "${DRIVEFLOW_CLANG_TIDY_COMMAND}")
    set_property(TARGET epoll_receiver_test PROPERTY CXX_CLANG_TIDY
                                                     "${DRIVEFLOW_CLANG_TIDY_COMMAND}")
    set_property(TARGET runtime_config_test PROPERTY CXX_CLANG_TIDY
                                                     "${DRIVEFLOW_CLANG_TIDY_COMMAND}")
    set_property(TARGET runtime_test PROPERTY CXX_CLANG_TIDY
                                              "${DRIVEFLOW_CLANG_TIDY_COMMAND}")
  endif()

  if(DRIVEFLOW_TSAN_DISABLE_ASLR)
    if(NOT DRIVEFLOW_SANITIZER STREQUAL "thread")
      message(FATAL_ERROR "DRIVEFLOW_TSAN_DISABLE_ASLR is only valid with the thread sanitizer")
    endif()
    find_program(DRIVEFLOW_SETARCH_EXECUTABLE NAMES setarch REQUIRED)
    set_property(
      TARGET protocol_packet_test
      PROPERTY CROSSCOMPILING_EMULATOR
               "${DRIVEFLOW_SETARCH_EXECUTABLE};${CMAKE_SYSTEM_PROCESSOR};-R")
    set_property(
      TARGET sensor_payload_test
      PROPERTY CROSSCOMPILING_EMULATOR
               "${DRIVEFLOW_SETARCH_EXECUTABLE};${CMAKE_SYSTEM_PROCESSOR};-R")
    set_property(
      TARGET sensor_simulator_test
      PROPERTY CROSSCOMPILING_EMULATOR
               "${DRIVEFLOW_SETARCH_EXECUTABLE};${CMAKE_SYSTEM_PROCESSOR};-R")
    set_property(
      TARGET udp_socket_test
      PROPERTY CROSSCOMPILING_EMULATOR
               "${DRIVEFLOW_SETARCH_EXECUTABLE};${CMAKE_SYSTEM_PROCESSOR};-R")
    set_property(
      TARGET epoll_receiver_test
      PROPERTY CROSSCOMPILING_EMULATOR
               "${DRIVEFLOW_SETARCH_EXECUTABLE};${CMAKE_SYSTEM_PROCESSOR};-R")
    set_property(
      TARGET runtime_config_test
      PROPERTY CROSSCOMPILING_EMULATOR
               "${DRIVEFLOW_SETARCH_EXECUTABLE};${CMAKE_SYSTEM_PROCESSOR};-R")
    set_property(
      TARGET runtime_test
      PROPERTY CROSSCOMPILING_EMULATOR
               "${DRIVEFLOW_SETARCH_EXECUTABLE};${CMAKE_SYSTEM_PROCESSOR};-R")
  endif()

  include(GoogleTest)
  gtest_discover_tests(protocol_packet_test DISCOVERY_MODE PRE_TEST)
  gtest_discover_tests(sensor_payload_test DISCOVERY_MODE PRE_TEST)
  gtest_discover_tests(sensor_simulator_test DISCOVERY_MODE PRE_TEST)
  gtest_discover_tests(udp_socket_test DISCOVERY_MODE PRE_TEST)
  gtest_discover_tests(epoll_receiver_test DISCOVERY_MODE PRE_TEST)
  gtest_discover_tests(runtime_config_test DISCOVERY_MODE PRE_TEST)
  gtest_discover_tests(runtime_test DISCOVERY_MODE PRE_TEST)
endif()

if(DRIVEFLOW_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "DRIVEFLOW_BUILD_FUZZERS requires Clang")
  endif()

  add_executable(decode_packet_fuzz tests/fuzz/decode_packet_fuzz.cpp)
  target_link_libraries(decode_packet_fuzz PRIVATE DriveFlow::Protocol)
  if(DRIVEFLOW_ENABLE_CLANG_TIDY)
    set_property(TARGET decode_packet_fuzz PROPERTY CXX_CLANG_TIDY
                                                   "${DRIVEFLOW_CLANG_TIDY_COMMAND}")
  endif()
  target_compile_options(decode_packet_fuzz PRIVATE -fsanitize=fuzzer,address)
  target_link_options(decode_packet_fuzz PRIVATE -fsanitize=fuzzer,address)
endif()
