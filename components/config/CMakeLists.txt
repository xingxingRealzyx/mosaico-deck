idf_component_register(
    SRCS "src/board.cpp"
    INCLUDE_DIRS "include"
    # board.cpp 要读 eFuse 判断板子版本（官方 BSP 也是这么做的）。
    PRIV_REQUIRES efuse log
)
