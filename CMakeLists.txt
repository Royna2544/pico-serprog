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
    drivers/i2cdrv.c
    drivers/pinctrldrv.c
    drivers/usbcdcdrv.c
    drivers/serprog.c
    drivers/latched-switch.c
    drivers/signal-action.c
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

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(NETWORK_MANAGER_LIB REQUIRED IMPORTED_TARGET libnm)
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
    find_package(Threads REQUIRED)
    add_library(esp-mock-sdk STATIC
        mock-esp8266-sdk/Arduino.cpp
        mock-esp8266-sdk/Wlan.cpp
        mock-esp8266-sdk/IPAddress.cpp
        mock-esp8266-sdk/Stream.cpp
        mock-esp8266-sdk/Sockets.cpp
        mock-esp8266-sdk/FS.cpp
        mock-esp8266-sdk/Crypto.cpp
        mock-esp8266-sdk/SPI.cpp
        mock-esp8266-sdk/vendor/ESP8266WebServer/detail/mimetable.cpp)
    target_compile_features(esp-mock-sdk PUBLIC cxx_std_17)
    target_include_directories(esp-mock-sdk PUBLIC mock-esp8266-sdk
        mock-esp8266-sdk/vendor/ESP8266WebServer)
    target_link_libraries(esp-mock-sdk PUBLIC boardstate-client
        PkgConfig::NETWORK_MANAGER_LIB OpenSSL::Crypto Threads::Threads)
    option(MOCK_NETWORK_SANITIZERS "Enable ASan/UBSan for the ESP host SDK" OFF)
    if(MOCK_NETWORK_SANITIZERS)
        target_compile_options(esp-mock-sdk PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(esp-mock-sdk PUBLIC -fsanitize=address,undefined)
    endif()
    include(CTest)
    if(BUILD_TESTING)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_executable(esp-network-test tests/esp-network.cpp)
        target_link_libraries(esp-network-test PRIVATE esp-mock-sdk)
        foreach(case support tcp pending peek-fragments ownership pressure udp multicast broadcast descriptors descriptor-zero reset)
            add_test(NAME esp-network-${case} COMMAND esp-network-test ${case})
            set_tests_properties(esp-network-${case} PROPERTIES TIMEOUT 15)
        endforeach()
        add_test(NAME esp-network-http COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test-esp-http.py $<TARGET_FILE:esp-network-test>)
        set_tests_properties(esp-network-http PROPERTIES TIMEOUT 60)
    endif()
    set_source_files_properties(esp8266/esp8266.ino PROPERTIES LANGUAGE CXX COMPILE_FLAGS "-x c++")
    add_executable(esp8266-mock mock-esp8266-sdk/main.cpp esp8266/esp8266.ino)
    target_link_libraries(esp8266-mock PRIVATE esp-mock-sdk)
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
    target_link_options(boardstate-wlan-host PRIVATE
        "LINKER:--script=${CMAKE_CURRENT_SOURCE_DIR}/driver.lds")
    set_property(TARGET boardstate-wlan-host APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/driver.lds")

    add_executable(pico_serprog main.c ${TINYLINUX_SOURCES} drivers/fakenorflash.cpp)
    target_link_libraries(pico_serprog PRIVATE pico-mock-sdk fdt boardstate-client)
    add_subdirectory(mock-pico-sdk)
    target_compile_definitions(pico_serprog PRIVATE MOCK_HARDWARE)
    target_compile_options(pico_serprog PRIVATE -fsanitize=undefined,address,leak)
    target_link_options(pico_serprog PRIVATE -fsanitize=undefined,address,leak)
endif()
# GCC defers a bare -T until after -Wl options, reversing the order relative to
# the SDK's memory-map script. INSERT scripts must precede that full script.
target_link_options(pico_serprog PRIVATE
    "LINKER:--script=${CMAKE_CURRENT_SOURCE_DIR}/driver.lds")
set_property(TARGET pico_serprog APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/driver.lds")

find_program(DTC dtc REQUIRED)

if (MOCK_HARDWARE)
set(DTS "${CMAKE_CURRENT_SOURCE_DIR}/dts/mock-board.dts")
else()
set(DTS "${CMAKE_CURRENT_SOURCE_DIR}/dts/board.dts")
endif()
set(COMMON_DTSI "${CMAKE_CURRENT_SOURCE_DIR}/dts/common.dtsi")
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
    DEPENDS "${DTS}" "${COMMON_DTSI}"
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

