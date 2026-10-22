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
)
target_include_directories(core PUBLIC core include)

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
