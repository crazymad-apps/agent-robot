cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components/network_connectivity")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(agent_robot_network_connectivity_validation)
