cmake_minimum_required(VERSION 3.20)

project(PhotoBridge LANGUAGES CXX)

find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(
    PHOTOBRIDGE_ENABLE_SANITIZERS
    "Enable AddressSanitizer and UndefinedBehaviorSanitizer"
    OFF
)

function(photobridge_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

function(photobridge_enable_sanitizers target)
    if(NOT PHOTOBRIDGE_ENABLE_SANITIZERS)
        return()
    endif()

    if(MSVC)
        target_compile_options(${target} PRIVATE /fsanitize=address)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(
            ${target}
            PRIVATE
                -fsanitize=address,undefined
                -fno-omit-frame-pointer
        )
    else()
        message(FATAL_ERROR
            "PHOTOBRIDGE_ENABLE_SANITIZERS is unsupported for this compiler")
    endif()
endfunction()

function(photobridge_enable_sanitizer_link target)
    if(NOT PHOTOBRIDGE_ENABLE_SANITIZERS)
        return()
    endif()

    if(MSVC)
        target_link_options(${target} PRIVATE /fsanitize=address)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_link_options(
            ${target}
            PRIVATE
                -fsanitize=address,undefined
        )
    endif()
endfunction()

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
    src/common/digest.cpp
    src/common/logging.cpp
    src/common/posix_error.cpp
    src/common/test_hooks.cpp
    src/common/time.cpp
    src/common/unique_fd.cpp

    src/model/relative_path.cpp
    src/model/physical_asset.cpp
    src/model/logical_asset.cpp
    src/model/loss_analysis.cpp
    src/model/capability.cpp
    src/model/path_mapper.cpp
    src/model/canonical_plan.cpp
    src/model/plan_artifact.cpp
    src/model/task.cpp
    src/model/asset_classifier.cpp
    src/model/association_edge.cpp
    src/model/association_graph.cpp
    src/model/metadata_candidate.cpp
    src/model/diff_engine.cpp
    src/model/audit_report.cpp
    src/model/metadata_rules.cpp
    src/model/metadata_resolver.cpp
    src/model/photo_ir.cpp
    src/model/provenance.cpp
    src/model/resolution_record.cpp

    src/app/workspace_layout.cpp
    src/app/workspace_service.cpp
    src/app/sqlite_connection.cpp
    src/app/sqlite_schema.cpp
    src/app/task_runtime_repository.cpp
    src/app/migration_attempt_preparer.cpp
    src/app/manifest_builder.cpp
    src/app/plan_execution_lock.cpp
    src/filesystem/mutation_guard.cpp
    src/filesystem/copy_and_hash.cpp
    src/filesystem/reconciler.cpp
    src/filesystem/binary_verifier.cpp

    src/cli/cli_app.cpp
    src/cli/dispatcher.cpp
    src/cli/init_command.cpp
    src/cli/pipeline_command.cpp
    src/pipeline/pipeline_router.cpp
    src/pipeline/pipeline_shared.cpp
    src/pipeline/scan_service.cpp
    src/pipeline/plan_service.cpp
    src/pipeline/migration_service.cpp
    src/pipeline/recovery_service.cpp
    src/pipeline/verify_service.cpp
    src/pipeline/status_service.cpp
    src/cli/serve_command.cpp
    src/cli/takeout_parse_command.cpp
)

add_library(photobridge_linux STATIC
    src/filesystem/linux_file_ops.cpp
    src/filesystem/linux_directory_walker.cpp
    src/filesystem/local_folder_source.cpp
    src/filesystem/serve_bootstrap.cpp
    src/filesystem/upload_page.cpp
    src/filesystem/http_server.cpp
    src/filesystem/upload_session_store.cpp
    src/source/takeout_parser.cpp
)

target_include_directories(photobridge_linux
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(photobridge_linux
    PUBLIC
        photobridge_core
        unofficial::nayuki-qr-code-generator::nayuki-qr-code-generator
)

target_include_directories(photobridge_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(photobridge_core
    PUBLIC
        spdlog::spdlog
        CLI11::CLI11
        nlohmann_json::nlohmann_json
        unofficial::sqlite3::sqlite3
        Threads::Threads
        BLAKE3::blake3
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
        unofficial::sqlite3::sqlite3
        xxHash::xxhash
        unofficial::nayuki-qr-code-generator::nayuki-qr-code-generator
)

photobridge_enable_warnings(photobridge_core)
photobridge_enable_warnings(photobridge_linux)
photobridge_enable_warnings(photobridge)

photobridge_enable_sanitizers(photobridge_core)
photobridge_enable_sanitizers(photobridge_linux)
photobridge_enable_sanitizers(photobridge)
photobridge_enable_sanitizer_link(photobridge)

enable_testing()
add_subdirectory(tests)

if(UNIX)
    add_executable(
        photobridge_bench
        benchmarks/photobridge_bench.cpp
    )

    target_include_directories(
        photobridge_bench
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    target_link_libraries(
        photobridge_bench
        PRIVATE
            photobridge_core
            photobridge_linux
            nlohmann_json::nlohmann_json
    )

    photobridge_enable_warnings(photobridge_bench)
    photobridge_enable_sanitizers(photobridge_bench)
    photobridge_enable_sanitizer_link(photobridge_bench)
endif()
