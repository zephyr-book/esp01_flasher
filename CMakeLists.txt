cmake_minimum_required(VERSION 3.20.0)

# The board (zbook/rp2350b/m33) comes from the `zbook` Zephyr module declared in
# west.yml — no BOARD_ROOT here. Select it at build time with `-b zbook/rp2350b/m33`.
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(bringup LANGUAGES C)

include_directories(./include)

file(GLOB_RECURSE SRC ${CMAKE_SOURCE_DIR} "src/*.c")
target_sources(app PRIVATE ${SRC})
