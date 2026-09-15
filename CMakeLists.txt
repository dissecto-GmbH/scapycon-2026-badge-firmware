cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

# firmware.ver / esp_app_desc version — YYYY.MM.DD.HH.MM.SS (local time).
# Override: PROJECT_VER=... idf.py reconfigure build
if(DEFINED ENV{PROJECT_VER} AND NOT "$ENV{PROJECT_VER}" STREQUAL "")
    set(PROJECT_VER "$ENV{PROJECT_VER}")
else()
    string(TIMESTAMP PROJECT_VER "%Y.%m.%d.%H.%M.%S")
endif()
message(STATUS "Badge PROJECT_VER=${PROJECT_VER}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(badge2026_v2x)
