FILE(GLOB cfiles *.c)
list(APPEND cfiles sim.c)
FILE(GLOB mongoose_files ../mongoose/*.c)
idf_component_register(SRCS ${cfiles} ${mongoose_files}
                       INCLUDE_DIRS "." "../mongoose" ".")


# Read version from root directory
file(READ ${CMAKE_SOURCE_DIR}/version.txt PROJECT_VERSION_FROM_FILE)
string(STRIP ${PROJECT_VERSION_FROM_FILE} PROJECT_VERSION_FROM_FILE)

# Add compile definition
target_compile_definitions(${COMPONENT_LIB} PRIVATE PROJECT_VERSION="${PROJECT_VERSION_FROM_FILE}")