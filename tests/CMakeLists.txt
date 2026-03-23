# logos-package-downloader tests

add_executable(lgpd_tests
    test_downloader.cpp
)

if(TARGET GTest::gtest)
    target_link_libraries(lgpd_tests PRIVATE
        package_downloader_lib
        GTest::gtest
        GTest::gtest_main
    )
else()
    target_link_libraries(lgpd_tests PRIVATE
        package_downloader_lib
        gtest
        gtest_main
    )
endif()

gtest_discover_tests(lgpd_tests)
