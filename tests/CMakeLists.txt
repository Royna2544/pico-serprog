cmake_minimum_required(VERSION 3.14)
project(PicoSerprogTests)

add_executable(testdrvmodel testdrvmodel.c ../device.c)
target_link_options(testdrvmodel PRIVATE ${CMAKE_EXE_LINKER_FLAGS} -T ${CMAKE_SOURCE_DIR}/../driver.lds)