# wizzairhelper_core: pure domain + algorithms (no I/O, no Qt).
# Domain values, Unicode text rules, exact decimals, strict JSON, civil time,
# locale-aware formatting, airport catalog, place matching, planner, pricing,
# result views, booking links, round-trip combiner, and the abstract
# clock / HTTP / cancellation contracts the services implement.

wizz_collect_sources(WIZZ_CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(wizzairhelper_core STATIC ${WIZZ_CORE_SOURCES})
add_library(wizz::core ALIAS wizzairhelper_core)

target_include_directories(wizzairhelper_core
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

# ICU ships with Windows 10 1903+ as icu.dll; the import library and <icu.h>
# come from the Windows SDK (02 §4.3).
target_link_libraries(wizzairhelper_core
    PUBLIC  wizz::project_options
    PRIVATE icu)
