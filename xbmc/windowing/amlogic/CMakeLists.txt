set(SOURCES WinSystemAmlogic.cpp
            VideoSyncAML.cpp
            AMLDisplay.cpp)

set(HEADERS WinSystemAmlogic.h
            VideoSyncAML.h
            AMLDisplay.h)

if(TARGET ${APP_NAME_LC}::OpenGLES)
  list(APPEND SOURCES WinSystemAmlogicGLESContext.cpp)
  list(APPEND HEADERS WinSystemAmlogicGLESContext.h)
endif()

core_add_library(windowing_Amlogic)
