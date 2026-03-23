# CLI executable for logos-package-downloader
cmake_minimum_required(VERSION 3.14)

set(CLI_SOURCES
    main.cpp
)

add_executable(lgpd ${CLI_SOURCES})

target_include_directories(lgpd PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(lgpd PRIVATE
    package_downloader_lib
    nlohmann_json::nlohmann_json
)

set_target_properties(lgpd PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Platform-specific RPATH settings
if(APPLE)
    set_target_properties(lgpd PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@executable_path/../lib"
    )
elseif(UNIX)
    set_target_properties(lgpd PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "$ORIGIN/../lib"
    )
endif()

install(TARGETS lgpd
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
