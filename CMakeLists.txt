cmake_minimum_required(VERSION 3.13)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

option(MOCK_HARDWARE "Mock hardware for testing" OFF)

include(FetchContent)

FetchContent_Declare(
    dtc
    GIT_REPOSITORY https://github.com/dgibson/dtc.git
    GIT_TAG v1.7.2
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(dtc)

set(LIBFDT_DIR "${dtc_SOURCE_DIR}/libfdt")

add_library(fdt STATIC
    "${LIBFDT_DIR}/fdt.c"
    "${LIBFDT_DIR}/fdt_ro.c"
    "${LIBFDT_DIR}/fdt_wip.c"
    "${LIBFDT_DIR}/fdt_sw.c"
    "${LIBFDT_DIR}/fdt_rw.c"
    "${LIBFDT_DIR}/fdt_strerror.c"
    "${LIBFDT_DIR}/fdt_empty_tree.c"
    "${LIBFDT_DIR}/fdt_addresses.c"
    "${LIBFDT_DIR}/fdt_overlay.c"
    "${LIBFDT_DIR}/fdt_check.c"
)

target_include_directories(fdt PUBLIC
    "${LIBFDT_DIR}"
)

set(TINYLINUX_SOURCES
    core/device.c
    core/printk.c
    core/devicetree.c
    core/panic.c
    core/irq.c
    drivers/uartdrv.c
    drivers/wlandrv.c
    drivers/leddrv.c
    drivers/spidrv.c
    drivers/gpiodrv.c
    drivers/gpiolib.c
    drivers/pinctrldrv.c
    drivers/usbcdcdrv.c
    drivers/serprog.c
    drivers/passthru.c
)

if (NOT MOCK_HARDWARE)
    include(pico_sdk_import.cmake)
    project(pico_serprog C CXX ASM)

    pico_sdk_init()

    add_executable(pico_serprog)
    target_sources(pico_serprog PRIVATE main.c drivers/usb/usb_descriptors.c core/init.c ${TINYLINUX_SOURCES})
    target_link_libraries(pico_serprog PRIVATE
        pico_stdlib
        pico_multicore
        hardware_spi
        hardware_uart
        hardware_i2c
        tinyusb_device
        fdt)
    pico_add_extra_outputs(pico_serprog)
    pico_enable_stdio_usb(pico_serprog 0)
    pico_enable_stdio_uart(pico_serprog 1)
else()
    project(pico_serprog_mock C CXX ASM)
    add_library(boardstate-client STATIC boardstate-daemon/client.c)
    target_include_directories(boardstate-client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    add_executable(boardstate-daemon boardstate-daemon/boardstate.cpp)
    target_link_libraries(boardstate-daemon PRIVATE fdt)

    add_library(esp-mock-sdk STATIC mock-esp8266-sdk/Arduino.cpp)
    target_include_directories(esp-mock-sdk PUBLIC mock-esp8266-sdk)
    target_link_libraries(esp-mock-sdk PUBLIC boardstate-client)

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(NETWORK_MANAGER_LIB REQUIRED IMPORTED_TARGET libnm)
    set_source_files_properties(esp8266/esp8266.ino PROPERTIES LANGUAGE CXX COMPILE_FLAGS "-x c++")
    add_executable(esp8266-mock mock-esp8266-sdk/main.cpp mock-esp8266-sdk/Wlan.cpp esp8266/esp8266.ino)
    target_link_libraries(esp8266-mock PRIVATE esp-mock-sdk PkgConfig::NETWORK_MANAGER_LIB)
    add_executable(esp-restart-probe mock-esp8266-sdk/main.cpp tests/esp-restart-probe.cpp)
    target_link_libraries(esp-restart-probe PRIVATE esp-mock-sdk)
    add_executable(boardstate-pico-smoke tests/boardstate-pico-smoke.c)
    target_link_libraries(boardstate-pico-smoke PRIVATE pico-mock-sdk)
    add_executable(boardstate-pico-irq tests/boardstate-pico-irq.c)
    target_link_libraries(boardstate-pico-irq PRIVATE pico-mock-sdk)
    add_executable(boardstate-wlan-host tests/boardstate-wlan-host.c
        ${TINYLINUX_SOURCES} drivers/fakenorflash.cpp)
    target_compile_definitions(boardstate-wlan-host PRIVATE MOCK_HARDWARE)
    target_link_libraries(boardstate-wlan-host PRIVATE pico-mock-sdk fdt)
    target_link_options(boardstate-wlan-host PRIVATE -T ${CMAKE_SOURCE_DIR}/driver.lds)

    add_executable(pico_serprog main.c ${TINYLINUX_SOURCES} drivers/fakenorflash.cpp)
    target_link_libraries(pico_serprog PRIVATE pico-mock-sdk fdt boardstate-client)
    add_subdirectory(mock-pico-sdk)
    target_compile_definitions(pico_serprog PRIVATE MOCK_HARDWARE)
    #target_compile_options(pico_serprog PRIVATE -fsanitize=undefined,address,leak)
    #target_link_options(pico_serprog PRIVATE -fsanitize=undefined,address,leak)
endif()
target_link_options(pico_serprog PRIVATE ${CMAKE_EXE_LINKER_FLAGS} -T ${CMAKE_SOURCE_DIR}/driver.lds)

find_program(DTC dtc REQUIRED)

set(DTS "${CMAKE_CURRENT_SOURCE_DIR}/board.dts")
set(DTB "${CMAKE_CURRENT_BINARY_DIR}/board.dtb")
set(DTB_ASM "${CMAKE_CURRENT_BINARY_DIR}/board_dtb.S")

# DTS -> DTB
add_custom_command(
    OUTPUT "${DTB}"
    COMMAND "${DTC}"
        -I dts
        -O dtb
        -o "${DTB}"
        "${DTS}"
    DEPENDS "${DTS}"
    COMMENT "Compiling device tree"
    VERBATIM
)

add_custom_target(devicetree_blob DEPENDS "${DTB}")

# Generate assembly with the DTB's absolute path
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/board_dtb.S.in"
    "${DTB_ASM}"
    @ONLY
)

# Embed into firmware
target_sources(pico_serprog PRIVATE "${DTB_ASM}")

# Ensure assembler runs again when DTB changes
set_source_files_properties("${DTB_ASM}"
    PROPERTIES OBJECT_DEPENDS "${DTB}"
)

add_dependencies(pico_serprog devicetree_blob)
if (MOCK_HARDWARE)
    add_dependencies(boardstate-daemon devicetree_blob)
    target_sources(boardstate-wlan-host PRIVATE "${DTB_ASM}")
    add_dependencies(boardstate-wlan-host devicetree_blob)
endif()

