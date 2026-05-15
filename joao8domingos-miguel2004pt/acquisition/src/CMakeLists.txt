message(STATUS "SRC CMake: Building target '${TARGET_SUBDIR}'")

idf_component_register(
    SRCS "${TARGET_SUBDIR}/main.cpp"
    INCLUDE_DIRS "${TARGET_SUBDIR}"
    REQUIRES "AcquisitionFramework"
)