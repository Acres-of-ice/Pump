cmake_minimum_required(VERSION 3.16)

# Auto-detect PCB configs (simcam style)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/pcb")
    file(GLOB pcb_cfgs "pcb/*.cfg")
    foreach(pcb_cfg ${pcb_cfgs})
        get_filename_component(pcb_name ${pcb_cfg} NAME_WE)
        list(APPEND EXTRA_BUILD_DIRECTORIES "${pcb_name}")
    endforeach()
endif()


# Read base version from version.txt (version management handled by increment_version.cmake)
set(VERSION_FILE ${CMAKE_SOURCE_DIR}/version.txt)
if(EXISTS ${VERSION_FILE})
    file(READ ${VERSION_FILE} FULL_VERSION)
    string(STRIP ${FULL_VERSION} FULL_VERSION)
    # Extract base version (everything before '+')
    string(REGEX REPLACE "\\+.*$" "" BASE_VERSION ${FULL_VERSION})
else()
    # Fallback if version.txt doesn't exist yet
    set(BASE_VERSION "0.0.0")
endif()

# Set project version (ESP-IDF uses semantic version without build metadata)
set(PROJECT_VER ${BASE_VERSION})

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Start with default project name
project(pump VERSION ${BASE_VERSION})
#project(pump)
