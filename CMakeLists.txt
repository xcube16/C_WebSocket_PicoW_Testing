cmake_minimum_required(VERSION 3.12)

set(PICO_BOARD pico_w)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
#add_compile_options(-save-temps=obj)

# Pull in SDK (must be before project)
# tasted with version 1.5.1 6a7db34ff63345a7badec79ebea3aaef1712f374
include(pico_sdk_import.cmake)

project(testing C CXX ASM)


# Initialize the SDK
pico_sdk_init()

# TODO: set in lwipopts.h
#add_definitions(-DLWIP_DEBUG=9)

add_custom_command(
    OUTPUT index_html.h
    COMMAND cp ${CMAKE_CURRENT_SOURCE_DIR}/index.html ${CMAKE_CURRENT_BINARY_DIR} && xxd -i index.html index_html.h
    DEPENDS index.html
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

add_executable(testing
    testing.c
    bufferless_str.c
    #lwipopts.h
    sub_task.S
    iol_lock.c
    index_html.h
)

pico_set_program_name(testing "PICO_TESTING")
pico_set_program_version(testing "0.1")

# no_flash means the target is to run from RAM
#pico_set_binary_type(testing no_flash)

pico_enable_stdio_uart(testing 1)
pico_enable_stdio_usb(testing 0)

# pull in common dependencies
target_link_libraries(testing
    pico_stdlib
    hardware_adc
    pico_cyw43_arch_lwip_threadsafe_background
    pico_mbedtls
)

# Add the standard include files to the build
target_include_directories(testing PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}
  ${CMAKE_CURRENT_LIST_DIR}/.. # for our common lwipopts or any other standard includes, if required
)

# create map/bin/hex file etc.
pico_add_extra_outputs(testing)
