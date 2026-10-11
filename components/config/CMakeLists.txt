idf_component_register(
    SRCS "src/board.cpp"
    INCLUDE_DIRS "include"
    # board.cpp needs to read the eFuse to determine the board revision (the official BSP does the same).
    PRIV_REQUIRES efuse log
)
