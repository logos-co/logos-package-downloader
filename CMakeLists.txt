cmake_minimum_required(VERSION 3.14)
project(PackageDownloaderLib LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find nlohmann_json
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    )
    FetchContent_MakeAvailable(json)
endif()

# Find libcurl
find_package(CURL REQUIRED)

# Library sources
set(LIB_SOURCES
    src/package_downloader_lib.cpp
    src/package_downloader_lib.h
    src/lgpd.cpp
    src/lgpd.h
)

# Create the shared library
add_library(package_downloader_lib SHARED ${LIB_SOURCES})

# Export macro for shared library
target_compile_definitions(package_downloader_lib PRIVATE LGPD_BUILD_SHARED=1)

set_target_properties(package_downloader_lib PROPERTIES
    OUTPUT_NAME "package_downloader_lib"
)

# Include directories
target_include_directories(package_downloader_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Link dependencies
target_link_libraries(package_downloader_lib PRIVATE
    nlohmann_json::nlohmann_json
    CURL::libcurl
)

# RPATH settings
set_target_properties(package_downloader_lib PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH FALSE
)

if(APPLE)
    set_target_properties(package_downloader_lib PROPERTIES
        INSTALL_RPATH "@loader_path"
        INSTALL_NAME_DIR "@rpath"
        BUILD_WITH_INSTALL_NAME_DIR TRUE
    )
    add_custom_command(TARGET package_downloader_lib POST_BUILD
        COMMAND install_name_tool -id "@rpath/libpackage_downloader_lib.dylib" $<TARGET_FILE:package_downloader_lib>
        COMMENT "Updating library paths for macOS"
    )
else()
    set_target_properties(package_downloader_lib PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        INSTALL_RPATH_USE_LINK_PATH FALSE
    )
endif()

# Set output directories
set_target_properties(package_downloader_lib PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# CLI executable
add_subdirectory(cmd)

# Testing (optional)
option(LGPD_BUILD_TESTS "Build tests" OFF)
if(LGPD_BUILD_TESTS)
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.14.0
        )
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()

    enable_testing()
    include(GoogleTest)
    add_subdirectory(tests)
endif()

# Install rules
install(TARGETS package_downloader_lib
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES src/package_downloader_lib.h src/lgpd.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
