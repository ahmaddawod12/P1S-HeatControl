cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/web")

# Only a personal USB bootstrap may seed credentials from the ignored local
# file. Generic releases have no compiled credentials and use device_cfg NVS.
option(P1S_BOOTSTRAP_CREDENTIALS "Seed device Wi-Fi from the personal setup file" OFF)
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(P1S_BOOTSTRAP_CREDENTIALS AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.secret")
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.secret")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(p1s_heat_controller)
