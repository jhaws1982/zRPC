###############################################################################
# CMakeLists.txt for zRPC library
#  - Creates a CMake target library named 'zRPC' (alias zRPC::zRPC)
#  - Installs headers, library and a CMake package: find_package(zRPC)
#  - CPack builds TGZ packages, plus DEB and RPM where dpkg/rpmbuild exist
###############################################################################
cmake_minimum_required(VERSION 3.16...3.31 FATAL_ERROR)

project(zRPC
        VERSION "0.1.0"
        DESCRIPTION "0MQ-based RPC client/server library with MessagePack support"
        HOMEPAGE_URL "https://github.com/jhaws1982/zRPC"
        LANGUAGES CXX
       )

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(ZRPC_TOP_LEVEL ON)
else()
  set(ZRPC_TOP_LEVEL OFF)
endif()

option(ZRPC_BUILD_TESTS "Enable build of unit test applications" ${ZRPC_TOP_LEVEL})
option(ZRPC_INSTALL "Generate install and package rules" ${ZRPC_TOP_LEVEL})
option(ZRPC_FETCH_DEPS
       "Download cppzmq and msgpack-cxx with CPM when they are not installed" ON)
option(ZRPC_ENABLE_COVERAGE "Instrument zRPC and its tests for coverage" OFF)
option(ZRPC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ${ZRPC_TOP_LEVEL})
option(BUILD_SHARED_LIBS "Build zRPC as a shared library" ON)

include(GNUInstallDirs)

# Setup default compiler flags
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Warnings apply to zRPC's own targets only; they are never passed on to
# projects that link against zRPC
set(zrpc_warnings -pedantic
                  -Wall
                  -Wextra
                  -Wconversion
                  -Wsign-conversion
                  -Wno-psabi
   )
if(ZRPC_WARNINGS_AS_ERRORS)
  list(APPEND zrpc_warnings -pedantic-errors -Werror)
endif()

###############################################################################
# Dependencies
#  - libzmq must be installed (it is in every distribution)
#  - cppzmq and msgpack-cxx are header-only. Installed copies are always
#    preferred; with ZRPC_FETCH_DEPS=ON, missing ones are downloaded with CPM.
#    Distribution and cross builds should use ZRPC_FETCH_DEPS=OFF.
###############################################################################
find_package(Threads REQUIRED)

if(ZRPC_FETCH_DEPS)
  include(cmake/CPM.cmake)
  set(CPM_USE_LOCAL_PACKAGES ON)

  CPMAddPackage(
    NAME cppzmq
    VERSION 4.8.1
    GIT_TAG v4.10.0
    GITHUB_REPOSITORY "zeromq/cppzmq"
    OPTIONS "CPPZMQ_BUILD_TESTS OFF"
  )

  CPMAddPackage(
    NAME msgpack-cxx
    VERSION 6.0.0
    GIT_TAG cpp-7.0.0
    GITHUB_REPOSITORY "msgpack/msgpack-c"
    OPTIONS "MSGPACK_BUILD_DOCS OFF" "MSGPACK_BUILD_TESTS OFF"
            "MSGPACK_BUILD_EXAMPLES OFF" "MSGPACK_CXX20 ON"
            "MSGPACK_USE_BOOST OFF"
  )
else()
  find_package(cppzmq 4.8.1 REQUIRED)
  find_package(msgpack-cxx 6.0.0 REQUIRED)
endif()

if(ZRPC_ENABLE_COVERAGE)
  # Instruments every target in this directory: the library and its tests
  include(cmake/CodeCoverage.cmake)
  append_coverage_compiler_flags()
endif()

###############################################################################
# zRPC library
###############################################################################
set(zrpc_public_headers include/zRPC.hpp
                        include/zRPCClient.inl
                        include/zRPCProtocol.hpp
                        include/zRPCPublisher.inl
                        include/zRPCServer.inl
                        include/zRPCSubscriber.inl
                        include/zRPCSupport.hpp
   )

add_library(${PROJECT_NAME})
add_library(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME})
target_sources(${PROJECT_NAME} PRIVATE src/zRPCClient.cpp
                                       src/zRPCProtocol.cpp
                                       src/zRPCServer.cpp
                                       src/zRPCPublisher.cpp
                                       src/zRPCSubscriber.cpp
                                       ${zrpc_public_headers}
              )
target_include_directories(${PROJECT_NAME} PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/zRPC>
)
target_link_libraries(${PROJECT_NAME} PUBLIC cppzmq msgpack-cxx Threads::Threads)
target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_20)
target_compile_options(${PROJECT_NAME} PRIVATE ${zrpc_warnings})
set_target_properties(${PROJECT_NAME} PROPERTIES
  VERSION ${PROJECT_VERSION}
  # Until 1.0 every minor release may break the ABI
  SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
)

###############################################################################
# Test applications
###############################################################################
if(ZRPC_BUILD_TESTS)
  enable_testing()

  foreach(app client clientDetached server publisher subscriber)
    add_executable(${app} tests/${app}.cpp)
    target_link_libraries(${app} PRIVATE ${PROJECT_NAME}::${PROJECT_NAME})
    target_compile_options(${app} PRIVATE ${zrpc_warnings})
  endforeach()

  add_executable(unittest tests/unit.cpp)
  target_link_libraries(unittest PRIVATE ${PROJECT_NAME}::${PROJECT_NAME})
  target_compile_options(unittest PRIVATE ${zrpc_warnings})
  add_test(NAME unittest COMMAND unittest)

  if(ZRPC_ENABLE_COVERAGE)
    setup_target_for_coverage_gcovr_xml(NAME ${PROJECT_NAME}_coverage
                                        EXECUTABLE unittest
                                        DEPENDENCIES unittest
                                        BASE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                                        EXCLUDE "tests"
                                       )
  endif()
endif()

###############################################################################
# Install rules and CMake package
###############################################################################
if(ZRPC_INSTALL)
  include(CMakePackageConfigHelpers)
  set(zrpc_cmake_dir ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME})

  install(TARGETS ${PROJECT_NAME}
          EXPORT ${PROJECT_NAME}Targets
          LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
                  COMPONENT runtime
                  NAMELINK_COMPONENT dev
          ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT dev
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT runtime
         )
  install(FILES ${zrpc_public_headers}
          DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zRPC
          COMPONENT dev
         )
  install(EXPORT ${PROJECT_NAME}Targets
          NAMESPACE ${PROJECT_NAME}::
          DESTINATION ${zrpc_cmake_dir}
          COMPONENT dev
         )

  configure_package_config_file(cmake/zRPCConfig.cmake.in
                                ${CMAKE_CURRENT_BINARY_DIR}/zRPCConfig.cmake
                                INSTALL_DESTINATION ${zrpc_cmake_dir}
                               )
  write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/zRPCConfigVersion.cmake
    COMPATIBILITY SameMinorVersion
  )
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/zRPCConfig.cmake
                ${CMAKE_CURRENT_BINARY_DIR}/zRPCConfigVersion.cmake
          DESTINATION ${zrpc_cmake_dir}
          COMPONENT dev
         )
  install(FILES LICENSE README.md
          DESTINATION ${CMAKE_INSTALL_DOCDIR}
          COMPONENT dev
         )

  #############################################################################
  # Packages: run cpack in the build directory
  #  - runtime: libzrpc<major>.<minor> (the shared library)
  #  - dev:     libzrpc-dev / libzrpc-devel (headers, CMake package)
  #############################################################################
  set(zrpc_abi ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})

  set(CPACK_PACKAGE_NAME "libzrpc")
  set(CPACK_PACKAGE_VENDOR "Jonathan Haws")
  set(CPACK_PACKAGE_CONTACT "Jonathan Haws <hawsjr@gmail.com>")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
  set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE)
  # Offer DEB/RPM wherever the tools to build them exist
  set(CPACK_GENERATOR "TGZ")
  find_program(ZRPC_DPKG_EXECUTABLE dpkg)
  find_program(ZRPC_RPMBUILD_EXECUTABLE rpmbuild)
  if(ZRPC_DPKG_EXECUTABLE)
    list(APPEND CPACK_GENERATOR "DEB")
  endif()
  if(ZRPC_RPMBUILD_EXECUTABLE)
    list(APPEND CPACK_GENERATOR "RPM")
  endif()
  set(CPACK_COMPONENTS_ALL runtime dev)
  set(CPACK_DEB_COMPONENT_INSTALL ON)
  set(CPACK_RPM_COMPONENT_INSTALL ON)

  # The package architecture defaults to that of the build machine. Cross
  # builds must name the target, e.g. -DZRPC_PACKAGE_ARCH=arm64 (Debian) or
  # aarch64 (RPM).
  set(ZRPC_PACKAGE_ARCH "" CACHE STRING "Package architecture for cross builds")
  if(ZRPC_PACKAGE_ARCH)
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE ${ZRPC_PACKAGE_ARCH})
    set(CPACK_RPM_PACKAGE_ARCHITECTURE ${ZRPC_PACKAGE_ARCH})
  else()
    # dpkg-shlibdeps adds libc/libstdc++ dependencies; it cannot inspect
    # binaries built for another architecture
    set(CPACK_DEBIAN_RUNTIME_PACKAGE_SHLIBDEPS ON)
  endif()

  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  set(CPACK_DEBIAN_RUNTIME_PACKAGE_NAME "libzrpc${zrpc_abi}")
  set(CPACK_DEBIAN_RUNTIME_PACKAGE_DEPENDS "libzmq5 (>= 4.3.2)")
  set(CPACK_DEBIAN_RUNTIME_PACKAGE_SECTION "libs")
  set(CPACK_DEBIAN_DEV_PACKAGE_NAME "libzrpc-dev")
  set(CPACK_DEBIAN_DEV_PACKAGE_DEPENDS
      "libzrpc${zrpc_abi} (= ${PROJECT_VERSION}), libzmq3-dev (>= 4.3.2), cppzmq-dev (>= 4.8.1), libmsgpack-cxx-dev (>= 6.0.0)")
  set(CPACK_DEBIAN_DEV_PACKAGE_SECTION "libdevel")

  set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
  set(CPACK_RPM_PACKAGE_LICENSE "MIT")
  set(CPACK_RPM_RUNTIME_PACKAGE_NAME "libzrpc")
  set(CPACK_RPM_RUNTIME_PACKAGE_REQUIRES "zeromq >= 4.3.2")
  set(CPACK_RPM_DEV_PACKAGE_NAME "libzrpc-devel")
  set(CPACK_RPM_DEV_PACKAGE_REQUIRES
      "libzrpc = ${PROJECT_VERSION}, zeromq-devel >= 4.3.2, cppzmq-devel >= 4.8.1, msgpack-cxx-devel >= 6.0.0")

  include(CPack)
endif()
