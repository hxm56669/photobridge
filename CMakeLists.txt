cmake_minimum_required(VERSION 3.20)

project(PhotoBridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(exiv2 CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(blake3 CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(xxHash CONFIG REQUIRED)
find_package(unofficial-nayuki-qr-code-generator CONFIG REQUIRED)

add_library(photobridge_core STATIC

    src/common/status.cpp
    src/common/logging.cpp
    src/common/posix_error.cpp
    src/common/unique_fd.cpp

    src/model/relative_path.cpp

    src/app/workspace_layout.cpp
    src/app/workspace_service.cpp

    src/cli/cli_app.cpp
    src/cli/dispatcher.cpp
    src/cli/init_command.cpp
    src/cli/scan_command.cpp
)

add_library(photobridge_linux STATIC
    src/filesystem/linux_file_ops.cpp
)

target_include_directories(photobridge_linux
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(photobridge_linux
    PUBLIC
        photobridge_core
)

target_include_directories(photobridge_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(photobridge_core
    PUBLIC
        spdlog::spdlog
        CLI11::CLI11
)

add_executable(photobridge
    src/main.cpp
)

target_link_libraries(photobridge
    PRIVATE
        photobridge_core
        photobridge_linux
        nlohmann_json::nlohmann_json
        Exiv2::exiv2lib
        httplib::httplib
        BLAKE3::blake3
        unofficial::sqlite3::sqlite3
        xxHash::xxhash
        unofficial::nayuki-qr-code-generator::nayuki-qr-code-generator
)

enable_testing()

find_package(GTest CONFIG REQUIRED)


add_executable(
    photobridge_tests

    tests/smoke_test.cpp
    tests/status_test.cpp
    tests/status_or_test.cpp
    tests/cli_test.cpp
    tests/workspace_layout_test.cpp
    tests/unique_fd_test.cpp
    tests/posix_error_test.cpp
    tests/linux_file_ops_test.cpp
    tests/relative_path_test.cpp
)

target_include_directories(
    photobridge_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(
    photobridge_tests
    PRIVATE
    photobridge_core
    photobridge_linux
    GTest::gtest_main
)


include(GoogleTest)

gtest_discover_tests(photobridge_tests)
