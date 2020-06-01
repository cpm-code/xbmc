if("aml" IN_LIST CORE_PLATFORM_NAME_LC)
  set(SOURCES RPProcessInfoAmlogic.cpp)
  set(HEADERS RPProcessInfoAmlogic.h)

  core_add_library(rp-process-amlogic)
endif()
