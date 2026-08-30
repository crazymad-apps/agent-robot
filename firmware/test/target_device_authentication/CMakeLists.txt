cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components"
    "${CMAKE_CURRENT_LIST_DIR}/../../managed_components/espressif__cjson"
    "${CMAKE_CURRENT_LIST_DIR}/../../managed_components/espressif__mdns"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(agent_robot_terminal_authentication_validation)
