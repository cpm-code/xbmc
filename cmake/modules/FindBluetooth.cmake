#.rst:
# FindBluetooth
# ---------
# Finds the Bluetooth library
#
# This will define the following target:
#
#   ${APP_NAME_LC}::Bluetooth   - The Bluetooth library

if(NOT TARGET ${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME})

  include(cmake/scripts/common/ModuleHelpers.cmake)

  set(${CMAKE_FIND_PACKAGE_NAME}_MODULE_LC bluetooth)
  set(${${CMAKE_FIND_PACKAGE_NAME}_MODULE_LC}_DISABLE_VERSION ON)

  SETUP_BUILD_VARS()

  find_package(PkgConfig ${SEARCH_QUIET})
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(${${CMAKE_FIND_PACKAGE_NAME}_SEARCH_NAME} bluez IMPORTED_TARGET ${SEARCH_QUIET})
  endif()

  if(${${CMAKE_FIND_PACKAGE_NAME}_SEARCH_NAME}_FOUND)
    add_library(${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME} ALIAS PkgConfig::${${CMAKE_FIND_PACKAGE_NAME}_SEARCH_NAME})

    get_target_property(_ALIASTARGET ${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME} ALIASED_TARGET)
    if(_ALIASTARGET)
      set(LIB_TARGET ${_ALIASTARGET})
    else()
      set(LIB_TARGET ${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME})
    endif()

    get_target_property(_bluetooth_link_libs ${LIB_TARGET} INTERFACE_LINK_LIBRARIES)
    if(NOT _bluetooth_link_libs MATCHES "(^|;)bluetooth($|;)" AND
       NOT _bluetooth_link_libs MATCHES "(^|;)LIBRARY::bluetooth($|;)" AND
       NOT _bluetooth_link_libs MATCHES "libbluetooth")
      find_library(BLUETOOTH_LIBRARY NAMES bluetooth)
      if(BLUETOOTH_LIBRARY)
        target_link_libraries(${LIB_TARGET} INTERFACE "${BLUETOOTH_LIBRARY}")
      endif()
    endif()

    set(${${CMAKE_FIND_PACKAGE_NAME}_MODULE}_COMPILE_DEFINITIONS HAVE_LIBBLUETOOTH)
    ADD_TARGET_COMPILE_DEFINITION()
  endif()
endif()
