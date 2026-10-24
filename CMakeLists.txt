cmake_minimum_required(VERSION 3.16)

project(OrderBook
    VERSION 1.0.0
    DESCRIPTION "Local Binance order book in real time"
    LANGUAGES CXX)

# ---- C++ standard -----------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Default build type -----------------------------------------------------
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Needed by VS Code / clangd / cpptools IntelliSense
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# FindBoost module was removed in CMake 3.30+; use BoostConfig instead
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

# ---- Dependencies -----------------------------------------------------------
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL    REQUIRED)
find_package(Boost   REQUIRED COMPONENTS system)

# nlohmann/json -- header only: prefer the CMake config, fall back to headers
find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp)
    if(NOT NLOHMANN_JSON_INCLUDE_DIR)
        message(FATAL_ERROR "nlohmann/json not found. Install the development package (Fedora: 'json-devel', Debian/Ubuntu: 'nlohmann-json3-dev').")
    endif()
endif()

# cpr -- try CMake config, then pkg-config, then the raw library
find_package(cpr CONFIG QUIET)
if(cpr_FOUND)
    set(CPR_TARGET cpr::cpr)
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(CPR IMPORTED_TARGET cpr)
        if(NOT CPR_FOUND)
            pkg_check_modules(CPR IMPORTED_TARGET libcpr)
        endif()
    endif()
    if(CPR_FOUND)
        set(CPR_TARGET PkgConfig::CPR)
    else()
        find_library(CPR_LIBRARY NAMES cpr)
        if(NOT CPR_LIBRARY)
            message(FATAL_ERROR "cpr not found. Install package 'cpr-devel'.")
        endif()
        set(CPR_TARGET ${CPR_LIBRARY})
    endif()
endif()

# websocketpp -- header only, only the include dir may need to be added
find_path(WEBSOCKETPP_INCLUDE_DIR websocketpp/client.hpp)

# ---- Executable -------------------------------------------------------------
add_executable(orderbook
    main.cpp
    OrderBook.cpp
    OrderBookClient.cpp
    TradingSystem.cpp
)

target_include_directories(orderbook PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(NOT nlohmann_json_FOUND AND NLOHMANN_JSON_INCLUDE_DIR)
    target_include_directories(orderbook PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
endif()

if(WEBSOCKETPP_INCLUDE_DIR)
    target_include_directories(orderbook PRIVATE ${WEBSOCKETPP_INCLUDE_DIR})
endif()

target_link_libraries(orderbook PRIVATE
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    Boost::system
    CURL::libcurl
    ${CPR_TARGET}
)

if(nlohmann_json_FOUND)
    target_link_libraries(orderbook PRIVATE nlohmann_json::nlohmann_json)
endif()

target_compile_options(orderbook PRIVATE -Wall -Wextra)

# ---- Install ----------------------------------------------------------------
install(TARGETS orderbook RUNTIME DESTINATION bin)
