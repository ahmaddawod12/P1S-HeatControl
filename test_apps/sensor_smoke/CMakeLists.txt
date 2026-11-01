cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components/board"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/app_types"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/temperature_sensor"
)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(p1s_heat_sensor_smoke)
