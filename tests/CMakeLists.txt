find_package(GTest CONFIG REQUIRED)

add_executable(photobridge_tests
    unit/common/digest_test.cpp
    unit/common/posix_error_test.cpp
    unit/common/smoke_test.cpp
    unit/common/status_or_test.cpp
    unit/common/status_test.cpp
    unit/common/unique_fd_test.cpp

    unit/model/asset_classifier_test.cpp
    unit/model/association_graph_test.cpp
    unit/model/audit_report_test.cpp
    unit/model/canonical_plan_test.cpp
    unit/model/capability_test.cpp
    unit/model/diff_engine_test.cpp
    unit/model/logical_asset_test.cpp
    unit/model/loss_analysis_test.cpp
    unit/model/metadata_candidate_test.cpp
    unit/model/metadata_resolver_test.cpp
    unit/model/metadata_rules_test.cpp
    unit/model/path_mapper_test.cpp
    unit/model/photo_ir_test.cpp
    unit/model/plan_artifact_test.cpp
    unit/model/provenance_test.cpp
    unit/model/relative_path_test.cpp
    unit/model/resolution_record_test.cpp
    unit/model/task_test.cpp
    unit/model/time_candidate_test.cpp

    unit/filesystem/binary_verifier_test.cpp
    unit/filesystem/copy_and_hash_test.cpp
    unit/filesystem/io_uring_copy_engine_test.cpp
    unit/filesystem/directory_walker_test.cpp
    unit/filesystem/linux_file_ops_test.cpp
    unit/filesystem/local_folder_source_test.cpp
    unit/filesystem/mutation_guard_test.cpp
    unit/filesystem/reconciler_test.cpp

    unit/app/serve_bootstrap_test.cpp
    unit/app/upload_page_test.cpp
    unit/app/upload_session_store_test.cpp
    unit/app/workspace_layout_test.cpp

    integration/cli/cli_test.cpp
    integration/pipeline/large_fixture_test.cpp
    integration/pipeline/live_photo_test.cpp
    integration/pipeline/raw_jpeg_test.cpp
    integration/pipeline/takeout_parser_test.cpp
    integration/sqlite/manifest_builder_test.cpp
    integration/sqlite/runtime_db_writer_test.cpp
    integration/sqlite/sqlite_connection_test.cpp
    integration/sqlite/sqlite_schema_test.cpp
    integration/sqlite/task_runtime_repository_test.cpp

    recovery/lock_recovery_test.cpp
)

# Keep the existing executable path for local scripts and IDE configurations.
set_target_properties(photobridge_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
)

target_include_directories(photobridge_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(photobridge_tests PRIVATE
    photobridge_core
    photobridge_linux
    GTest::gtest_main
)

photobridge_enable_warnings(photobridge_tests)
photobridge_enable_sanitizers(photobridge_tests)
photobridge_enable_sanitizer_link(photobridge_tests)

include(GoogleTest)
gtest_discover_tests(photobridge_tests
    WORKING_DIRECTORY "${PROJECT_BINARY_DIR}"
)
