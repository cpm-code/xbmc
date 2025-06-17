#.rst:
# FindAML
# -------
# Finds the AML codec
#
# This will define the following variables::
#
# AML_INCLUDE_DIRS - the AML include directory
# AML_DEFINITIONS - the AML definitions
#
# and the following imported targets::
#
#   ${APP_NAME_LC}::AML   - The AML codec

find_path(AML_INCLUDE_DIR codec_error.h
                          PATH_SUFFIXES amcodec)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(AML
                                  REQUIRED_VARS AML_INCLUDE_DIR)

include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES ${AML_INCLUDE_DIR})

if(AML_FOUND)
  set(AML_INCLUDE_DIRS ${AML_INCLUDE_DIR})
  set(AML_DEFINITIONS -DHAS_LIBAMCODEC=1)

  if(NOT TARGET ${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME})
    add_library(${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME} INTERFACE IMPORTED)
    set_target_properties(${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME} PROPERTIES
                                   INTERFACE_INCLUDE_DIRECTORIES "${AML_INCLUDE_DIR}"
                                   INTERFACE_COMPILE_DEFINITIONS "${AML_DEFINITIONS}")
  endif()
endif()

mark_as_advanced(AMLCODEC_INCLUDE_DIR)
