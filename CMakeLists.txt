cmake_minimum_required(VERSION 3.16)

# Auto-detect PCB configs (simcam style)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/pcb")
    file(GLOB pcb_cfgs "pcb/*.cfg")
    foreach(pcb_cfg ${pcb_cfgs})
        get_filename_component(pcb_name ${pcb_cfg} NAME_WE)
        list(APPEND EXTRA_BUILD_DIRECTORIES "${pcb_name}")
    endforeach()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pump)
