
CMAKE_MINIMUM_REQUIRED(VERSION 2.8.0)
set(PROJECT_NAME_STR libuv-test)
project(${PROJECT_NAME_STR} C CXX)

#-------------------------------------------------------------------------------
# The version number
#-------------------------------------------------------------------------------
set (libuv-test_VERSION_MAJOR 0)
set (libuv-test_VERSION_MINOR 1)
set (libuv-test_VERSION_STRING ${libuv-test_VERSION_MAJOR}.${libuv-test_VERSION_MINOR})

#-------------------------------------------------------------------------------
# setup deps
#-------------------------------------------------------------------------------
set(EXT_PREFIX ${PROJECT_SOURCE_DIR}/ext)
set(CMAKE_MODULE_PATH ${EXT_PREFIX})

# OS specific lib stuff
if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
  set(Boost_USE_STATIC_LIBS ON)
  set(Boost_USE_STATIC_RUNTIME ON)
else()
  set(Boost_USE_STATIC_LIBS OFF)
  set(Boost_USE_STATIC_RUNTIME OFF)
endif()

# Boost
set(Boost_USE_MULTITHREADED ON)
find_package(Boost 1.47.0 )
set(INCLUDES ${INCLUDES} ${Boost_INCLUDE_DIRS} )
set(LIBS ${LIBS} ${Boost_LIBRARIES} )

# v8
set(LIBUV_INCLUDE_NAMES uv.h)
set(LIBUV_LIBRARY_NAMES libuv.dylib)

find_path(LIBUV_INCLUDE_DIRS NAMES ${LIBUV_INCLUDE_NAMES} HINTS /usr/include /usr/local/include)
find_library(LIBUV_LIBRARIES NAMES ${LIBUV_LIBRARY_NAMES} HINTS /usr/include /usr/local/include)

if(LIBUV_INCLUDE_DIRS AND LIBUV_LIBRARIES)
  set(LIBUV_FOUND TRUE)
  set(INCLUDES ${INCLUDES} ${LIBUV_INCLUDE_DIR} )
  set(LIBS ${LIBS} ${LIBUV_LIBRARIES} )
  message(STATUS "Found LIBUV: ${LIBUV_LIBRARIES}")
else(LIBUV_INCLUDE_DIRS AND LIBUV_LIBRARIES)
  message(FATAL_ERROR "Could not find LIBUV library.")
endif(LIBUV_INCLUDE_DIRS AND LIBUV_LIBRARIES)

#-------------------------------------------------------------------------------
# libuv-test static, shared and executable
#-------------------------------------------------------------------------------
set(PROJECT_LIB_NAME ${PROJECT_NAME_STR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "build/bin/")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/build/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/build/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/build/bin)
set(PROJECT_INCLUDE_DIR ${PROJECT_SOURCE_DIR}/include)

include_directories(${INCLUDES} ${PROJECT_INCLUDE_DIR} ${PROJECT_SOURCE_DIR}/src)
file(GLOB SRC_FILES ${PROJECT_SOURCE_DIR}/src/*.cpp)
file(GLOB SRC_FILES ${PROJECT_SOURCE_DIR}/src/*.c)

add_executable(${PROJECT_NAME} ${SRC_FILES})

target_link_libraries(${PROJECT_LIB_NAME} ${LIBS})
SET_TARGET_PROPERTIES(${PROJECT_LIB_NAME} PROPERTIES VERSION ${libuv-test_VERSION_MAJOR}.${libuv-test_VERSION_MINOR} SOVERSION ${libuv-test_VERSION_MAJOR})

#-------------------------------------------------------------------------------
# Set compiler flags
#-------------------------------------------------------------------------------
set(PROJECT_COMPILER_FLAGS "${CMAKE_CXX_FLAGS} -fPIC -Wall -pedantic -Wextra -Werror -Wno-long-long")

set_property(
  TARGET ${PROJECT_NAME}
  APPEND PROPERTY COMPILE_FLAGS ${PROJECT_COMPILER_FLAGS})
