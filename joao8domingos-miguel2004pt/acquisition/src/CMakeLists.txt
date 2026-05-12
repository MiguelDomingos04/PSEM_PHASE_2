if(DEFINED ENV{BOARD} AND "$ENV{BOARD}" STREQUAL "board2")
    set(BOARD_SRC "board2.cpp")
else()
    set(BOARD_SRC "board1.cpp")
endif()

idf_component_register(SRCS "${BOARD_SRC}")