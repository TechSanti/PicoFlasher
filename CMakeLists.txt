cmake_minimum_required(VERSION 3.13)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(PicoFlasher_Corona_Stable C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

option(USE_RP2040_ZERO "Use the RP2040-Zero pin map and onboard WS2812" OFF)
option(ENABLE_DEBUG_UART "Enable UART stdio diagnostics" OFF)

set(EMMC_TRANSFER_HZ 6250000 CACHE STRING
    "Corona 4 GB eMMC transfer clock in Hz")
set(EMMC_FALLBACK_HZ 3125000 CACHE STRING
    "Automatic fallback clock after repeated eMMC read/CRC failures")
set(XBOX_SPI_HZ 18000000 CACHE STRING
    "Corona 16 MB SMC/NAND SPI clock in Hz")

# Old build directories can retain the previous 12.5 MHz cache value.  This
# stability edition never permits an eMMC clock above the proven 6.25 MHz.
if(EMMC_TRANSFER_HZ GREATER 6250000)
    message(WARNING
        "Cached EMMC_TRANSFER_HZ=${EMMC_TRANSFER_HZ} is unsafe; forcing 6250000")
    set(EMMC_TRANSFER_HZ 6250000 CACHE STRING
        "Corona 4 GB eMMC transfer clock in Hz" FORCE)
endif()

if(EMMC_FALLBACK_HZ GREATER 3125000)
    message(WARNING
        "Cached EMMC_FALLBACK_HZ=${EMMC_FALLBACK_HZ} is unsafe; forcing 3125000")
    set(EMMC_FALLBACK_HZ 3125000 CACHE STRING
        "Automatic fallback clock after repeated eMMC read/CRC failures" FORCE)
endif()

pico_sdk_init()

add_executable(${PROJECT_NAME}
    main.c
    usb_descriptors.c
    spiex.c
    xbox.c
    pio_spi.c
    nuvoton_spi.c
    isd1200.c
    sdio.c
)

if(USE_RP2040_ZERO)
    target_compile_definitions(${PROJECT_NAME} PRIVATE BOARD_RP2040_ZERO=1)
    message(STATUS "PicoFlasher: RP2040-Zero pin map")
else()
    message(STATUS "PicoFlasher: standard Raspberry Pi Pico pin map")
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE
    EMMC_TRANSFER_HZ=${EMMC_TRANSFER_HZ}
    EMMC_FALLBACK_HZ=${EMMC_FALLBACK_HZ}
    XBOX_SPI_HZ=${XBOX_SPI_HZ}
)

target_compile_options(${PROJECT_NAME} PRIVATE
    -Wall
    -Wextra
    -Wshadow
    -Wno-unused-parameter
)

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_LIST_DIR})

pico_generate_pio_header(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/spi.pio)
pico_generate_pio_header(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/sdio.pio)
pico_generate_pio_header(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/ws2812.pio)

target_link_libraries(${PROJECT_NAME}
    pico_stdlib
    tinyusb_device_unmarked
    hardware_clocks
    hardware_pio
    hardware_spi
    hardware_dma
)

pico_enable_stdio_usb(${PROJECT_NAME} 0)

if(ENABLE_DEBUG_UART)
    pico_enable_stdio_uart(${PROJECT_NAME} 1)
else()
    pico_enable_stdio_uart(${PROJECT_NAME} 0)
endif()

pico_add_extra_outputs(${PROJECT_NAME})
