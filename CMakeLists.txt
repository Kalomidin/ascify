cmake_minimum_required(VERSION 3.16.8)

project(ascify-clang)

include(GNUInstallDirs)

option(ASCIFY_CLANG_TESTS "Build ASCIFY tests, if lit is installed" OFF)
option(ASCIFY_CLANG_TESTS_ONLY "Build ASCIFY tests only, if lit is installed and ascify-clang binary is already produced" OFF)
option(ASCIFY_INCLUDE_IN_ASCIFY_SDK "Include ASCIFY in ASCIFY SDK" OFF)
option(ASCIFY_INSTALL_CLANG_HEADERS "Include clang headers in ASCIFY installation" ON)

if(NOT WIN32)
  set(CPACK_SET_DESTDIR ON CACHE BOOL "Installer package will install ascify-clang to CMAKE_INSTALL_PREFIX instead of CPACK_PACKAGING_INSTALL_PREFIX")
endif()

if(ASCIFY_CLANG_TESTS OR ASCIFY_CLANG_TESTS_ONLY)
  set(ASCIFY_TEST "ON")
else()
  set(ASCIFY_TEST "OFF")
endif()

if(ASCIFY_CLANG_TESTS_ONLY)
  set(ASCIFY_BUILD "OFF")
else()
  set(ASCIFY_BUILD "ON")
endif()

message(STATUS "ASCIFY config:")
message(STATUS "   - Build ascify-clang    : ${ASCIFY_BUILD}")
message(STATUS "   - Test ascify-clang     : ${ASCIFY_TEST}")
message(STATUS "   - Is part of ASCIFY SDK    : ${ASCIFY_INCLUDE_IN_ASCIFY_SDK}")
message(STATUS "   - Install clang headers : ${ASCIFY_INSTALL_CLANG_HEADERS}")

if(ASCIFY_INCLUDE_IN_ASCIFY_SDK)
  if(NOT WIN32)
    message(FATAL_ERROR "ASCIFY_INCLUDE_IN_ASCIFY_SDK is only supported on Windows")
  elseif(CMAKE_GENERATOR MATCHES "Visual Studio")
    message(FATAL_ERROR "ASCIFY_INCLUDE_IN_ASCIFY_SDK is not targeting Visual Studio")
  endif()
else()
  find_package(Clang REQUIRED CONFIG PATHS ${CMAKE_PREFIX_PATH})
  find_package(LLVM REQUIRED CONFIG PATHS ${CMAKE_PREFIX_PATH})

  message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}:")
  message(STATUS "   - CMake module path     : ${LLVM_CMAKE_DIR}")
  message(STATUS "   - Clang include path    : ${CLANG_INCLUDE_DIRS}")
  message(STATUS "   - LLVM Include path     : ${LLVM_INCLUDE_DIRS}")
  message(STATUS "   - Binary path           : ${LLVM_TOOLS_BINARY_DIR}")
endif()

list(APPEND CMAKE_MODULE_PATH ${LLVM_CMAKE_DIR})
include(AddLLVM)

if (NOT ASCIFY_CLANG_TESTS_ONLY)
  if(MSVC AND MSVC_VERSION VERSION_LESS "1900")
    message(SEND_ERROR "ascify-clang could be built by Visual Studio 14 2015 or higher.")
    return()
  endif()

  include_directories(${CLANG_INCLUDE_DIRS})
  include_directories(${LLVM_INCLUDE_DIRS})
  add_definitions(${LLVM_DEFINITIONS})

  file(GLOB_RECURSE ASCIFY_SOURCES src/*.cpp)
  file(GLOB_RECURSE ASCIFY_HEADERS src/*.h)

  # In order to be compatible with an LLVM that uses LLVM_LINK_LLVM_DYLIB=ON,
  # ascify-clang must link core LLVM libraries via LLVM_LINK_COMPONENTS vs directly adding
  # them.
  set(ASCIFY_ADDL_LINK_LIBS)
  set(LLVM_LINK_COMPONENTS
    ProfileData
    Support
    MCParser
    MC
    BitReader
    Option
    Core
  )
  
  if(LLVM_PACKAGE_VERSION VERSION_GREATER "6.0.1")
    list(APPEND ASCIFY_ADDL_LINK_LIBS clangToolingInclusions)
  endif()

  if(LLVM_PACKAGE_VERSION VERSION_GREATER "9.0.1")
    list(APPEND LLVM_LINK_COMPONENTS FrontendOpenMP)
  endif()

  if(LLVM_PACKAGE_VERSION VERSION_EQUAL "15.0.0" OR LLVM_PACKAGE_VERSION VERSION_GREATER "15.0.0")
    list(APPEND LLVM_LINK_COMPONENTS WindowsDriver)
    list(APPEND ASCIFY_ADDL_LINK_LIBS clangSupport)
  endif()


  add_llvm_executable(ascify-clang
    ${ASCIFY_SOURCES} ${ASCIFY_HEADERS}
  )
  target_link_directories(ascify-clang PRIVATE ${LLVM_LIBRARY_DIRS})

  if(ASCIFY_INCLUDE_IN_ASCIFY_SDK)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
      message(FATAL_ERROR "In order to include ASCIFY in ASCIFY SDK, ASCIFY needs to be built with LLVM_EXTERNAL_PROJECTS")
    endif()

    # Need to add clang include directories explicitly if building as part of llvm.
    if(LLVM_EXTERNAL_CLANG_SOURCE_DIR)
      target_include_directories(ascify-clang
        PRIVATE
          ${LLVM_BINARY_DIR}/tools/clang/include
          ${LLVM_EXTERNAL_CLANG_SOURCE_DIR}/include)
    endif()

    # Need to add lld include directories explicitly if building as part of llvm.
    if(LLVM_EXTERNAL_LLD_SOURCE_DIR)
      target_include_directories(ascify-clang
        PRIVATE
          ${LLVM_BINARY_DIR}/tools/lld/include
          ${LLVM_EXTERNAL_LLD_SOURCE_DIR}/include)
    endif()

  # Keep the existing compiler since MSVC-specific flags will be set later.
  elseif(MSVC)
  else()
    # Overriding CMAKE_CXX_COMPILER/CMAKE_C_COMPILER below is sketchy - could
    # that be removed or reworked instead?
    set(CMAKE_CXX_COMPILER ${LLVM_TOOLS_BINARY_DIR}/clang++)
    set(CMAKE_C_COMPILER ${LLVM_TOOLS_BINARY_DIR}/clang)
  endif()

  # Link against LLVM and CLANG libraries.
  target_link_libraries(ascify-clang PRIVATE
    clangASTMatchers
    clangFrontend
    clangTooling
    clangParse
    clangSerialization
    clangSema
    clangEdit
    clangFormat
    clangLex
    clangAnalysis
    clangDriver
    clangAST
    clangToolingCore
    clangRewrite
    clangBasic
    ${ASCIFY_ADDL_LINK_LIBS}
  )

  if(LLVM_PACKAGE_VERSION VERSION_EQUAL "5.0.0" OR LLVM_PACKAGE_VERSION VERSION_GREATER "5.0.0")
    if(MSVC)
      set(STD "/std:c++17")
    else()
      set(STD "-std=c++17")
    endif()
  elseif(LLVM_PACKAGE_VERSION VERSION_GREATER "3.4.0")
    if(MSVC)
      set(STD "/std:c++14")
    else()
      set(STD "-std=c++14")
    endif()
  endif()

  if(MSVC)
    target_link_libraries(ascify-clang PRIVATE version)
    string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    target_compile_options(ascify-clang PRIVATE ${STD} /Od /GR- /EHs- /EHc- /MP)
    if((LLVM_PACKAGE_VERSION VERSION_EQUAL "18.0.0" OR LLVM_PACKAGE_VERSION VERSION_GREATER "18.0.0") AND MSVC_VERSION VERSION_GREATER "1919")
      target_compile_options(ascify-clang PRIVATE /Zc:preprocessor)
    endif()
    set(CMAKE_CXX_LINK_FLAGS "${CMAKE_CXX_LINK_FLAGS} /SUBSYSTEM:WINDOWS")
  else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${STD} -pthread -fno-rtti -fvisibility-inlines-hidden")
  endif()

  # Address Sanitize Flag.
  if(ADDRESS_SANITIZER)
    set(addr_var -fsanitize=address)
  else()
    set(addr_var )
  endif()
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${EXTRA_CFLAGS} ${addr_var}")

  if(LLVM_PACKAGE_VERSION VERSION_EQUAL "16.0.0" OR LLVM_PACKAGE_VERSION VERSION_GREATER "16.0.0")
    set(LIB_CLANG_RES ${LLVM_VERSION_MAJOR})
  else()
    set(LIB_CLANG_RES ${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}.${LLVM_VERSION_PATCH})
  endif()
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${EXTRA_CFLAGS} -DLIB_CLANG_RES=${LIB_CLANG_RES} ${addr_var}")

  set(INSTALL_PATH_DOC_STRING "ascify-clang Installation Path")
  if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${PROJECT_BINARY_DIR}/dist" CACHE PATH ${INSTALL_PATH_DOC_STRING} FORCE)
  endif()

  set(ASCIFY_BIN_INSTALL_PATH "${CMAKE_INSTALL_PREFIX}/bin")

  set(ASCIFY_LIBEXEC_INSTALL_PATH "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBEXECDIR}/ascify")

  install(TARGETS ascify-clang DESTINATION bin)


  # Headers are already included in HIP SDK, so skip those if including ASCIFY in HIP SDK.
  if( ASCIFY_INSTALL_CLANG_HEADERS)
    if(NOT ASCIFY_INCLUDE_IN_HIP_SDK)
      # Install all folders under clang/version/ in CMAKE_INSTALL_PREFIX path.
      install(
        DIRECTORY ${LLVM_LIBRARY_DIRS}/clang/${LIB_CLANG_RES}/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ascify/${CMAKE_INSTALL_INCLUDEDIR}
        COMPONENT clang-resource-headers
        FILES_MATCHING
        PATTERN "*.h"
        PATTERN "*.modulemap"
        PATTERN "algorithm"
        PATTERN "complex"
        PATTERN "new"
        PATTERN "ppc_wrappers" EXCLUDE
        PATTERN "omp-tools.h" EXCLUDE
        PATTERN "omp.h" EXCLUDE
        PATTERN "ompt.h" EXCLUDE
        PATTERN "openmp_wrappers" EXCLUDE)
    endif()
  endif()

  if(UNIX)
    # Get rid of any RPATH definations already.
    set_target_properties(ascify-clang PROPERTIES INSTALL_RPATH "")
    # Set RPATH for the binary.
    # For macOS, use this instead:
    if(APPLE)
        set_target_properties(ascify-clang PROPERTIES LINK_FLAGS "-Wl,-rpath,@loader_path/../lib")
    else()
        set_target_properties(ascify-clang PROPERTIES LINK_FLAGS "-Wl,--enable-new-dtags -Wl,--rpath,\\$ORIGIN/../lib")
    endif()

    # set(CPACK_PACKAGING_INSTALL_PREFIX "/opt/rocm" CACHE PATH "HIP Package Installation Path")
    # set(BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/packages/ascify-clang)

    # configure_file(packaging/ascify-clang.txt ${BUILD_DIR}/CMakeLists.txt @ONLY)
    # configure_file(${CMAKE_SOURCE_DIR}/LICENSE.txt ${BUILD_DIR}/LICENSE.txt @ONLY)

    add_custom_target(package_ascify-clang COMMAND ${CMAKE_COMMAND} .
      WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
      COMMAND rm -rf *.deb *.rpm *.tar.gz
      COMMAND make package
      COMMAND cp *.deb ${PROJECT_BINARY_DIR}
      COMMAND cp *.rpm ${PROJECT_BINARY_DIR}
      COMMAND cp *.tar.gz ${PROJECT_BINARY_DIR}
      WORKING_DIRECTORY ${BUILD_DIR})
  endif()

endif() # if (NOT ASCIFY_CLANG_TESTS_ONLY)

if(ASCIFY_CLANG_TESTS OR ASCIFY_CLANG_TESTS_ONLY)

  message(STATUS "---- The below configuring for ascify-clang testing only ----")

  set (ASCIFY_CLANG_RES "${LLVM_LIBRARY_DIRS}/clang/${LIB_CLANG_RES}")

  if(${CMAKE_VERSION} VERSION_LESS "3.27.0")
    find_package(PythonInterp 3.0 REQUIRED)
  else()
    find_package(Python 3.0...3.15 REQUIRED)
  endif()

  function (require_program PROGRAM_NAME)
    find_program(FOUND_${PROGRAM_NAME} ${PROGRAM_NAME})
    if(FOUND_${PROGRAM_NAME})
       message(STATUS "Found ${PROGRAM_NAME}: ${FOUND_${PROGRAM_NAME}}")
    else()
       message(SEND_ERROR "Can't find ${PROGRAM_NAME}. Either set ASCIFY_CLANG_TESTS(_ONLY) to OFF to disable ASCIFY tests, or install the missing program.")
    endif()
  endfunction()

  require_program(lit)
  require_program(FileCheck)

  set(CUDA_TOOLKIT_ROOT_DIR "" CACHE PATH "Path to CUDA Toolkit to use in ascify-clang unit testing")
  set(CUDA_SDK_ROOT_DIR     "" CACHE PATH "Path to CUDA Toolkit Samples")
  set(CUDA_DNN_ROOT_DIR     "" CACHE PATH "Path to cuDNN")
  set(CUDA_TENSOR_ROOT_DIR  "" CACHE PATH "Path to cuTENSOR")
  set(CUDA_CUB_ROOT_DIR     "" CACHE PATH "Path to CUB")
  set(CUDA_FILE_ROOT_DIR    "" CACHE PATH "Path to cuFile")

  if(NOT CUDA_TOOLKIT_ROOT_DIR STREQUAL "" AND NOT CUDA_TOOLKIT_ROOT_DIR MATCHES "OFF")
    if(NOT DEFINED CUDAToolkit_ROOT OR CUDAToolkit_ROOT MATCHES "OFF")
      set(CUDAToolkit_ROOT "${CUDA_TOOLKIT_ROOT_DIR}")
    endif()
  else()
    if(DEFINED CUDAToolkit_ROOT AND NOT CUDAToolkit_ROOT MATCHES "OFF")
      set(CUDA_TOOLKIT_ROOT_DIR "${CUDAToolkit_ROOT}")
    endif()
  endif()

  message(STATUS "Initial CUDA to configure:")
  message(STATUS "   - CUDA Toolkit path     : ${CUDA_TOOLKIT_ROOT_DIR}")
  message(STATUS "   - CUDA Samples path     : ${CUDA_SDK_ROOT_DIR}")
  message(STATUS "   - cuDNN path            : ${CUDA_DNN_ROOT_DIR}")
  message(STATUS "   - cuTENSOR path         : ${CUDA_TENSOR_ROOT_DIR}")
  message(STATUS "   - CUB path              : ${CUDA_CUB_ROOT_DIR}")
  message(STATUS "   - cuFile path           : ${CUDA_FILE_ROOT_DIR}")

  if(${CMAKE_VERSION} VERSION_LESS "3.27.0")
    find_package(CUDA REQUIRED)
    set(CUDA_VERSION_FULL     "${CUDA_VERSION}")
  else()
    find_package(CUDAToolkit REQUIRED)
    set(CUDA_VERSION_MAJOR    "${CUDAToolkit_VERSION_MAJOR}")
    set(CUDA_VERSION_MINOR    "${CUDAToolkit_VERSION_MINOR}")
    set(CUDA_VERSION_PATCH    "${CUDAToolkit_VERSION_PATCH}")
    set(CUDA_VERSION_FULL     "${CUDAToolkit_VERSION}")
    set(CUDA_VERSION          "${CUDAToolkit_VERSION_MAJOR}.${CUDAToolkit_VERSION_MINOR}")
    set(CUDA_TOOLKIT_ROOT_DIR "${CUDAToolkit_LIBRARY_ROOT}")
  endif()

  if(CUDA_SDK_ROOT_DIR STREQUAL "")
    if(WIN32)
      if(CUDA_VERSION VERSION_LESS "11.6")
        set(NVCUDASAMPLES "NVCUDASAMPLES${CUDA_VERSION_MAJOR}_${CUDA_VERSION_MINOR}_ROOT")
        if(DEFINED ENV{${NVCUDASAMPLES}})
          set(CUDA_SDK_ROOT_DIR "$ENV{${NVCUDASAMPLES}}")
          string(REPLACE "\\" "/" CUDA_SDK_ROOT_DIR ${CUDA_SDK_ROOT_DIR})
        endif()
      endif()
    elseif(UNIX)
      if(NOT CUDA_TOOLKIT_ROOT_DIR STREQUAL "" AND NOT CUDA_TOOLKIT_ROOT_DIR STREQUAL "OFF" AND CUDA_VERSION VERSION_LESS "10.1")
        set(CUDA_SDK_ROOT_DIR "${CUDA_TOOLKIT_ROOT_DIR}/samples")
      endif()
    endif()
  endif()
  if(CUDA_SDK_ROOT_DIR STREQUAL "")
    set(CUDA_SDK_ROOT_DIR OFF)
  endif()

  if(CUDA_DNN_ROOT_DIR STREQUAL "")
    set(CUDA_DNN_ROOT_DIR OFF)
  else()
    set(CUDNN_VERSION_FILE_NAMES
        cudnn_version.h
        cudnn_version_v9.h
        cudnn_version_v999.h
    )
    unset(CUDNN_VERSION_FILE CACHE)
    find_file(CUDNN_VERSION_FILE NAMES ${CUDNN_VERSION_FILE_NAMES} PATHS ${CUDA_DNN_ROOT_DIR} NO_DEFAULT_PATH)
    if(NOT DEFINED CUDNN_VERSION_FILE OR CUDNN_VERSION_FILE STREQUAL "CUDNN_VERSION_FILE-NOTFOUND")
      unset(CUDNN_VERSION_FILE CACHE)
      find_file(CUDNN_VERSION_FILE NAMES ${CUDNN_VERSION_FILE_NAMES} PATHS ${CUDA_DNN_ROOT_DIR}/include NO_DEFAULT_PATH)
      if(DEFINED CUDNN_VERSION_FILE)
        set(CUDA_DNN_ROOT_DIR "${CUDA_DNN_ROOT_DIR}/include")
      endif()
    endif()
  endif()

  if(CUDA_TENSOR_ROOT_DIR STREQUAL "")
    set(CUDA_TENSOR_ROOT_DIR OFF)
  else()
    unset(CUDA_TENSOR_FILE CACHE)
    find_file(CUDA_TENSOR_FILE NAMES cutensor.h PATHS ${CUDA_TENSOR_ROOT_DIR} NO_DEFAULT_PATH)
    if(NOT DEFINED CUDA_TENSOR_FILE OR CUDA_TENSOR_FILE STREQUAL "CUDA_TENSOR_FILE-NOTFOUND")
      unset(CUDA_TENSOR_FILE CACHE)
      find_file(CUDA_TENSOR_FILE NAMES cutensor.h PATHS ${CUDA_TENSOR_ROOT_DIR}/include NO_DEFAULT_PATH)
      if(DEFINED CUDA_TENSOR_FILE)
        set(CUDA_TENSOR_ROOT_DIR "${CUDA_TENSOR_ROOT_DIR}/include")
      endif()
    endif()
  endif()

  if(CUDA_CUB_ROOT_DIR STREQUAL "")
    if(CUDA_VERSION VERSION_GREATER_EQUAL "11.0")
      if(WIN32 OR (UNIX AND CUDA_VERSION VERSION_LESS "11.4" OR CUDA_VERSION VERSION_GREATER_EQUAL "11.6"))
        if (CUDA_VERSION VERSION_GREATER_EQUAL "13.0")
          set(CUDA_CUB_ROOT_DIR "${CUDA_TOOLKIT_ROOT_DIR}/include/cccl")
        else()
          set(CUDA_CUB_ROOT_DIR "${CUDA_TOOLKIT_ROOT_DIR}/include")
        endif()
      else()
        set(CUDA_CUB_ROOT_DIR OFF)
      endif()
    else()
      set(CUDA_CUB_ROOT_DIR OFF)
    endif()
  endif()

  if(CUDA_FILE_ROOT_DIR STREQUAL "")
    set(CUDA_FILE_ROOT_DIR OFF)
  else()
    set(CUDA_FILE_ROOT_DIR "${CUDA_FILE_ROOT_DIR}/include")
  endif()

  message(STATUS "Found CUDA config:")
  message(STATUS "   - CUDA Toolkit path     : ${CUDA_TOOLKIT_ROOT_DIR}")
  message(STATUS "   - CUDA Samples path     : ${CUDA_SDK_ROOT_DIR}")
  message(STATUS "   - cuDNN path            : ${CUDA_DNN_ROOT_DIR}")
  message(STATUS "   - cuTENSOR path         : ${CUDA_TENSOR_ROOT_DIR}")
  message(STATUS "   - CUB path              : ${CUDA_CUB_ROOT_DIR}")
  message(STATUS "   - cuFile path           : ${CUDA_FILE_ROOT_DIR}")

  if((CUDA_VERSION VERSION_LESS "7.0") OR (LLVM_PACKAGE_VERSION VERSION_LESS "3.8") OR
     (CUDA_VERSION VERSION_GREATER "7.5" AND LLVM_PACKAGE_VERSION VERSION_LESS "4.0") OR
     (CUDA_VERSION VERSION_GREATER "8.0" AND LLVM_PACKAGE_VERSION VERSION_LESS "6.0") OR
     (CUDA_VERSION VERSION_GREATER "9.0" AND LLVM_PACKAGE_VERSION VERSION_LESS "7.0") OR
     (CUDA_VERSION VERSION_GREATER "9.2" AND LLVM_PACKAGE_VERSION VERSION_LESS "8.0") OR
     (CUDA_VERSION VERSION_GREATER "10.0" AND LLVM_PACKAGE_VERSION VERSION_LESS "9.0") OR
     (CUDA_VERSION VERSION_GREATER "10.1" AND LLVM_PACKAGE_VERSION VERSION_LESS "10.0"))
    message(SEND_ERROR "CUDA ${CUDA_VERSION} is not supported by LLVM ${LLVM_PACKAGE_VERSION}.")
    if(CUDA_VERSION_MAJOR VERSION_LESS "7")
      message(STATUS "Please install CUDA 7.0 or higher.")
    elseif(CUDA_VERSION_MAJOR VERSION_LESS "8")
      message(STATUS "Please install LLVM + clang 3.8 or higher.")
    elseif(CUDA_VERSION_MAJOR VERSION_LESS "9")
      message(STATUS "Please install LLVM + clang 4.0 or higher.")
    elseif(CUDA_VERSION VERSION_EQUAL "9.0")
      message(STATUS "Please install LLVM + clang 6.0 or higher.")
    elseif(CUDA_VERSION_MAJOR VERSION_LESS "10")
      message(STATUS "Please install LLVM + clang 7.0 or higher.")
    elseif(CUDA_VERSION VERSION_EQUAL "10.0")
      message(STATUS "Please install LLVM + clang 8.0 or higher.")
    elseif(CUDA_VERSION VERSION_EQUAL "10.1")
      message(STATUS "Please install LLVM + clang 9.0 or higher.")
    elseif(CUDA_VERSION VERSION_EQUAL "10.2" OR CUDA_VERSION VERSION_EQUAL "11.0")
      message(STATUS "Please install LLVM + clang 10.0 or higher.")
    endif()
  endif()

  configure_file(
    ${CMAKE_CURRENT_LIST_DIR}/tests/lit.site.cfg.in
    ${CMAKE_CURRENT_BINARY_DIR}/tests/lit.site.cfg
    @ONLY)

  if(NOT LIT_ARGS)
    set(LIT_ARGS -v)
  endif()

  if(ASCIFY_CLANG_TESTS_ONLY)
    add_lit_testsuite(test-ascify "Running ASCIFY regression tests"
      ${CMAKE_CURRENT_LIST_DIR}/tests
      PARAMS site_config=${CMAKE_CURRENT_BINARY_DIR}/tests/lit.site.cfg
      ARGS ${LIT_ARGS})
  else()
    add_lit_testsuite(test-ascify "Running ASCIFY regression tests"
      ${CMAKE_CURRENT_LIST_DIR}/tests
      PARAMS site_config=${CMAKE_CURRENT_BINARY_DIR}/tests/lit.site.cfg
      ARGS ${LIT_ARGS}
      DEPENDS ascify-clang)
  endif()

  add_custom_target(test-ascify-clang)
  add_dependencies(test-ascify-clang test-ascify)
  set_target_properties(test-ascify-clang PROPERTIES FOLDER "Tests")

endif() # if(ASCIFY_CLANG_TESTS OR ASCIFY_CLANG_TESTS_ONLY)
