# Flashloader (factory app) — Scapycon 2026 badge OTA helper
cmake_minimum_required(VERSION 3.22)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")
set(PROJECT_VER "fl-1.0.0")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(badge2026_flashloader)
