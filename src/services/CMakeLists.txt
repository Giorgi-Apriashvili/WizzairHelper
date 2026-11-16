# wizzairhelper_services: everything that performs I/O.
# libcurl transport (Schannel, Windows trust store, Windows proxy settings),
# file caches and AppPaths, request gate, Wizz Air client (discovery,
# classification, retries), currency service, geocoding / place resolver,
# the flight search service that runs a leg on a worker thread, and the
# self-updater core (GitHub releases, verified downloads, schedule).

find_package(CURL CONFIG REQUIRED)

wizz_collect_sources(WIZZ_SERVICES_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(wizzairhelper_services STATIC ${WIZZ_SERVICES_SOURCES})
add_library(wizz::services ALIAS wizzairhelper_services)

target_include_directories(wizzairhelper_services
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(wizzairhelper_services
    PUBLIC  wizz::core
    PRIVATE CURL::libcurl
            winhttp     # WinHttpGetIEProxyConfigForCurrentUser (Windows proxy settings)
            shell32     # SHGetKnownFolderPath
            ole32       # CoTaskMemFree
            bcrypt)     # SHA-256 of update downloads (Windows CNG)
