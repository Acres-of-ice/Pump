FILE(GLOB cfiles *.c)
list(APPEND cfiles sim.c)
FILE(GLOB mongoose_files ../mongoose/*.c)
idf_component_register(SRCS ${cfiles} ${mongoose_files}
                       INCLUDE_DIRS "." "../mongoose" ".")


