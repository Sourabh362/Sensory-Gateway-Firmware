cmake_minimum_required(VERSION 3.16)
project(fluxton_gateway C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Strict warnings: every warning is a build error.
add_compile_options(
    -Wall -Wextra -Werror -pedantic
    -Wshadow -Wstrict-prototypes -Wmissing-prototypes
)

include_directories(include)

find_package(Threads REQUIRED)

# Firmware core: portable, no OS calls, no heap.
add_library(core STATIC
    core/spsc_ring.c
    core/crc16.c
    core/reading.c
    core/parser.c
    core/flashlog.c
    core/app.c
)
target_include_directories(core PUBLIC core include)

# Simulated hardware: implements hal.h on a PC with threads and files.
add_library(sim STATIC
    sim/sim_time.c
    sim/sim_uart.c
    sim/sim_tick.c
    sim/sim_console.c
    sim/sim_flash.c
)
target_include_directories(sim PUBLIC sim include)
target_compile_definitions(sim PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(sim PUBLIC Threads::Threads)

add_executable(gateway sim/main.c)
target_link_libraries(gateway PRIVATE core sim)

enable_testing()

# add_unit_test(<name> <sources...> [LIBS <libs...>])
# Builds tests/<name>.c into an executable and registers it with CTest.
function(add_unit_test name)
    cmake_parse_arguments(T "" "" "LIBS" ${ARGN})
    add_executable(${name} tests/${name}.c ${T_UNPARSED_ARGUMENTS})
    target_include_directories(${name} PRIVATE tests)
    target_link_libraries(${name} PRIVATE ${T_LIBS})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_unit_test(test_smoke)
add_unit_test(test_spsc_ring LIBS core Threads::Threads)
add_unit_test(test_parser LIBS core)
add_unit_test(test_flash_sim LIBS sim)
add_unit_test(test_flashlog LIBS core sim)
