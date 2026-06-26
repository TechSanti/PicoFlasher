# Set minimum required version of CMake
cmake_minimum_required(VERSION 3.12)

# Include build functions from Pico SDK
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

# Set name of project (as PROJECT_NAME) and C/C++ standards
project(PicoFlasher C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# ====================================================================
# OPÇÃO HÍBRIDA: Escolha a placa alvo (Desativado = Pico Padrão)
# ====================================================================
option(USE_RP2040_ZERO "Build for RP2040-Zero instead of Standard Pico" OFF)

if(USE_RP2040_ZERO)
    add_compile_definitions(BOARD_RP2040_ZERO)
    message(STATUS ">>> Configurado para pinagem do RP2040-Zero <<<")
else()
    message(STATUS ">>> Configurado para pinagem do Raspberry Pi Pico Padrão <<<")
endif()
# ====================================================================

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

pico_add_extra_outputs(${PROJECT_NAME})

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_LIST_DIR})

pico_generate_pio_header(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/spi.pio)
pico_generate_pio_header(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/sdio.pio)
# NOVA LINHA AQUI PARA O LED RGB DO ZERO:
pico_generate_pio_header(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/ws2812.pio)

target_link_libraries(${PROJECT_NAME} 
    pico_stdlib
    tinyusb_device_unmarked
    hardware_pio
    hardware_spi
    hardware_dma
)

pico_enable_stdio_usb(${PROJECT_NAME} 0)
pico_enable_stdio_uart(${PROJECT_NAME} 1)