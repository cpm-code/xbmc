set(SOURCES WinSystemAmlogic.cpp
            VideoSyncAML.cpp
            DolbyVisionAML.cpp)

set(HEADERS WinSystemAmlogic.h
            VideoSyncAML.h
            DolbyVisionAML.h)

if(OPENGLES_FOUND)
  list(APPEND SOURCES WinSystemAmlogicGLESContext.cpp)
  list(APPEND HEADERS WinSystemAmlogicGLESContext.h)
endif()

core_add_library(windowing_Amlogic)
