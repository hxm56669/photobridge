#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "photobridge/app/manifest_builder.h"
#include "photobridge/app/sqlite_schema.h"
#include "photobridge/app/sqlite_statement.h"
#include "photobridge/cli/cli_app.h"
#include "photobridge/common/digest.h"
#include "photobridge/filesystem/copy_and_hash.h"
#include "photobridge/filesystem/io_uring_copy_engine.h"
#include "photobridge/filesystem/linux_directory_walker.h"
#include "photobridge/filesystem/linux_file_ops.h"
#include "photobridge/model/asset_classifier.h"

namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

struct Options {
    std::string workload = "all";
    std::filesystem::path data_root =
        std::filesystem::temp_directory_path() / "photobridge_bench_data";
    std::filesystem::path output;
    std::size_t repetitions = 5;
    std::size_t warmup_repetitions = 0;
    std::size_t scanner_files = 100'000;
    std::size_t copy_bytes = 64U * 1024U * 1024U;
    std::size_t copy_uring_qd = 4;
    std::size_t sqlite_commands = 5'000;
    std::size_t e2e_files = 4;
    std::size_t e2e_file_bytes = 64U * 1024U;
    std::size_t e2e_workers = 4;
    bool keep_data = false;
};

struct IoCounters {
    std::uint64_t rchar = 0;
    std::uint64_t wchar = 0;
    std::uint64_t read_bytes = 0;
    std::uint64_t write_bytes = 0;
    std::uint64_t syscr = 0;
    std::uint64_t syscw = 0;
};

struct ResourceSnapshot {
    double cpu_seconds = 0.0;
    std::uint64_t max_rss_kib = 0;
    IoCounters io;
};

struct Sample {
    double wall_ms = 0.0;
    double cpu_ms = 0.0;
    std::uint64_t max_rss_kib = 0;
    std::uint64_t rss_delta_kib = 0;
    IoCounters io;
    std::uint64_t items = 0;
    std::uint64_t bytes = 0;
};

struct WorkloadResult {
    std::string name;
    std::string unit;
    std::uint64_t configured_items = 0;
    std::uint64_t configured_bytes = 0;
    std::vector<Sample> samples;
    std::optional<std::size_t> e2e_workers;
    std::optional<std::string> implementation;
};

struct RunValues {
    std::uint64_t items = 0;
    std::uint64_t bytes = 0;
};

[[noreturn]] void Fail(std::string_view message)
{
    throw std::runtime_error(std::string(message));
}

template <typename T>
T Require(photobridge::StatusOr<T> result, std::string_view operation)
{
    if (!result.ok()) {
        throw std::runtime_error(
            std::string(operation) + ": " + result.status().message());
    }
    return std::move(result.value());
}

void Require(photobridge::Status status, std::string_view operation)
{
    if (!status.ok()) {
        throw std::runtime_error(
            std::string(operation) + ": " + status.message());
    }
}

std::uint64_t ParseUnsigned(std::string_view raw, std::string_view option)
{
    if (raw.empty()) {
        Fail(std::string(option) + " requires a positive integer");
    }
    std::uint64_t value = 0;
    for (const char character : raw) {
        if (character < '0' || character > '9') {
            Fail(std::string(option) + " requires a positive integer");
        }
        const std::uint64_t digit =
            static_cast<std::uint64_t>(character - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            Fail(std::string(option) + " is too large");
        }
        value = value * 10 + digit;
    }
    if (value == 0) {
        Fail(std::string(option) + " requires a positive integer");
    }
    return value;
}

std::uint64_t ParseNonNegative(std::string_view raw, std::string_view option)
{
    if (raw.empty()) {
        Fail(std::string(option) + " requires a non-negative integer");
    }
    std::uint64_t value = 0;
    for (const char character : raw) {
        if (character < '0' || character > '9') {
            Fail(std::string(option) + " requires a non-negative integer");
        }
        const std::uint64_t digit =
            static_cast<std::uint64_t>(character - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            Fail(std::string(option) + " is too large");
        }
        value = value * 10 + digit;
    }
    return value;
}

Options ParseOptions(int argc, char** argv)
{
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto value_for = [&](std::string_view option) -> std::string {
            if (index + 1 >= argc) {
                Fail(std::string(option) + " requires a value");
            }
            return argv[++index];
        };

        if (argument == "--help" || argument == "-h") {
            std::cout
                << "Usage: photobridge_bench [options]\n"
                << "  --workload all|scanner|copy|copy-sync|copy-uring|copy-uring-qd{1,2,4,8,16}|copy-compare|sqlite|sqlite-v1|sqlite-v2|sqlite-prepared|sqlite-batch-{10,100,1000,5000}|sqlite-compare|e2e|e2e-large|e2e-v6a|e2e-v6a-matrix\n"
                << "  --data-root PATH       benchmark-owned data directory\n"
                << "  --output PATH          write JSON report\n"
                << "  --repetitions N        measured samples (default 5)\n"
                << "  --warmup-repetitions N unmeasured E2E samples (default 0)\n"
                << "  --scanner-files N      files per scan (default 100000)\n"
                << "  --copy-bytes N         payload bytes (default 67108864)\n"
                << "  --copy-uring-qd N      io_uring queue depth, 1-16 (default 4)\n"
                << "  --sqlite-commands N    queued commands (default 5000)\n"
                << "  --e2e-files N          files per local pipeline (default 4)\n"
                << "  --e2e-file-bytes N     bytes per local file (default 65536)\n"
                << "  --e2e-workers N        migrate workers, 1-8 (default 4)\n"
                << "  --keep-data            preserve generated fixture\n";
            std::exit(0);
        }
        if (argument == "--keep-data") {
            options.keep_data = true;
        } else if (argument == "--workload") {
            options.workload = value_for(argument);
        } else if (argument == "--data-root") {
            options.data_root = value_for(argument);
        } else if (argument == "--output") {
            options.output = value_for(argument);
        } else if (argument == "--repetitions") {
            options.repetitions = static_cast<std::size_t>(
                ParseUnsigned(value_for(argument), argument));
        } else if (argument == "--warmup-repetitions") {
            options.warmup_repetitions = static_cast<std::size_t>(
                ParseNonNegative(value_for(argument), argument));
        } else if (argument == "--scanner-files") {
            options.scanner_files = static_cast<std::size_t>(
                ParseUnsigned(value_for(argument), argument));
        } else if (argument == "--copy-bytes") {
            options.copy_bytes = static_cast<std::size_t>(
                ParseUnsigned(value_for(argument), argument));
        } else if (argument == "--copy-uring-qd") {
            const std::uint64_t depth = ParseUnsigned(value_for(argument), argument);
            if (depth > 16) Fail("--copy-uring-qd must be between 1 and 16");
            options.copy_uring_qd = static_cast<std::size_t>(depth);
        } else if (argument == "--sqlite-commands") {
            options.sqlite_commands = static_cast<std::size_t>(
                ParseUnsigned(value_for(argument), argument));
        } else if (argument == "--e2e-files") {
            options.e2e_files = static_cast<std::size_t>(
                ParseUnsigned(value_for(argument), argument));
        } else if (argument == "--e2e-file-bytes") {
            options.e2e_file_bytes = static_cast<std::size_t>(
                ParseUnsigned(value_for(argument), argument));
        } else if (argument == "--e2e-workers") {
            const std::uint64_t workers = ParseUnsigned(value_for(argument), argument);
            if (workers > 8) Fail("--e2e-workers must be between 1 and 8");
            options.e2e_workers = static_cast<std::size_t>(workers);
        } else {
            Fail("unknown option: " + argument);
        }
    }

    if (options.workload != "all" && options.workload != "scanner"
        && options.workload != "copy" && options.workload != "copy-sync"
        && options.workload != "copy-uring"
        && options.workload != "copy-uring-qd1"
        && options.workload != "copy-uring-qd2"
        && options.workload != "copy-uring-qd4"
        && options.workload != "copy-uring-qd8"
        && options.workload != "copy-uring-qd16"
        && options.workload != "copy-compare"
        && options.workload != "sqlite"
        && options.workload != "sqlite-v1"
        && options.workload != "sqlite-v2"
        && options.workload != "sqlite-prepared"
        && options.workload != "sqlite-batch-10"
        && options.workload != "sqlite-batch-100"
        && options.workload != "sqlite-batch-1000"
        && options.workload != "sqlite-batch-5000"
        && options.workload != "sqlite-compare"
        && options.workload != "e2e" && options.workload != "e2e-large"
        && options.workload != "e2e-v6a"
        && options.workload != "e2e-v6a-matrix") {
        Fail("unknown workload; see --help for supported workloads");
    }
    return options;
}

std::uint64_t CounterDelta(std::uint64_t after, std::uint64_t before)
{
    return after >= before ? after - before : 0;
}

IoCounters ReadIoCounters()
{
    IoCounters counters;
    std::ifstream input("/proc/self/io");
    if (!input) {
        return counters;
    }
    std::string key;
    std::uint64_t value = 0;
    while (input >> key >> value) {
        if (key == "rchar:") counters.rchar = value;
        else if (key == "wchar:") counters.wchar = value;
        else if (key == "read_bytes:") counters.read_bytes = value;
        else if (key == "write_bytes:") counters.write_bytes = value;
        else if (key == "syscr:") counters.syscr = value;
        else if (key == "syscw:") counters.syscw = value;
    }
    return counters;
}

ResourceSnapshot SnapshotResources()
{
    struct rusage usage {};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) {
        Fail("getrusage failed");
    }
    return ResourceSnapshot{
        static_cast<double>(usage.ru_utime.tv_sec)
            + static_cast<double>(usage.ru_utime.tv_usec) / 1'000'000.0
            + static_cast<double>(usage.ru_stime.tv_sec)
            + static_cast<double>(usage.ru_stime.tv_usec) / 1'000'000.0,
        static_cast<std::uint64_t>(usage.ru_maxrss),
        ReadIoCounters(),
    };
}

Sample Measure(const std::function<RunValues()>& operation)
{
    const ResourceSnapshot before = SnapshotResources();
    const auto started = Clock::now();
    const RunValues values = operation();
    const auto finished = Clock::now();
    const ResourceSnapshot after = SnapshotResources();

    Sample sample;
    sample.wall_ms = std::chrono::duration<double, std::milli>(
        finished - started).count();
    sample.cpu_ms = (after.cpu_seconds - before.cpu_seconds) * 1000.0;
    sample.max_rss_kib = after.max_rss_kib;
    sample.rss_delta_kib = CounterDelta(
        after.max_rss_kib,
        before.max_rss_kib);
    sample.io = {
        CounterDelta(after.io.rchar, before.io.rchar),
        CounterDelta(after.io.wchar, before.io.wchar),
        CounterDelta(after.io.read_bytes, before.io.read_bytes),
        CounterDelta(after.io.write_bytes, before.io.write_bytes),
        CounterDelta(after.io.syscr, before.io.syscr),
        CounterDelta(after.io.syscw, before.io.syscw),
    };
    sample.items = values.items;
    sample.bytes = values.bytes;
    return sample;
}

void WriteDeterministicFile(const std::filesystem::path& path, std::size_t bytes)
{
    const int fd = ::open(
        path.c_str(),
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
        0600);
    if (fd < 0) {
        Fail("open copy benchmark source failed");
    }
    std::array<std::byte, 1U * 1024U * 1024U> buffer{};
    for (std::size_t offset = 0; offset < bytes;) {
        const std::size_t count = std::min(buffer.size(), bytes - offset);
        for (std::size_t index = 0; index < count; ++index) {
            buffer[index] = static_cast<std::byte>(
                (offset + index) % 251U);
        }
        std::size_t written = 0;
        while (written < count) {
            const ssize_t result = ::write(
                fd,
                buffer.data() + written,
                count - written);
            if (result < 0) {
                if (errno == EINTR) continue;
                ::close(fd);
                Fail("write copy benchmark source failed");
            }
            if (result == 0) {
                ::close(fd);
                Fail("copy benchmark source write made no progress");
            }
            written += static_cast<std::size_t>(result);
        }
        offset += count;
    }
    if (::fsync(fd) != 0) {
        ::close(fd);
        Fail("fsync copy benchmark source failed");
    }
    if (::close(fd) != 0) {
        Fail("close copy benchmark source failed");
    }
}

struct ScanSink final : photobridge::DirectoryEntrySink {
    explicit ScanSink(photobridge::ManifestBuilder& builder)
        : builder(builder) {}

    photobridge::Status Add(photobridge::DirectoryEntry entry) override
    {
        const auto asset = photobridge::ClassifyPhysicalAsset(entry);
        if (!asset.has_value()) return photobridge::Status::Ok();
        ++asset_count;
        return builder.Add(*asset);
    }

    photobridge::ManifestBuilder& builder;
    std::uint64_t asset_count = 0;
};

void RemoveDatabaseFiles(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.string() + "-wal", error);
    std::filesystem::remove(path.string() + "-shm", error);
}

void CreateScannerFixture(
    const std::filesystem::path& root,
    std::size_t file_count)
{
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error) Fail("create scanner fixture directory failed");
    for (std::size_t index = 0; index < file_count; ++index) {
        const auto path = root / ("photo-" + std::to_string(index) + ".jpg");
        const int fd = ::open(
            path.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
            0600);
        if (fd < 0) Fail("create scanner fixture file failed");
        if (::close(fd) != 0) Fail("close scanner fixture file failed");
    }
}

WorkloadResult RunScanner(
    const Options& options,
    const std::filesystem::path& fixture_root)
{
    WorkloadResult result{
        "scanner_manifest",
        "files/s",
        options.scanner_files,
        0,
        {},
        {},
        {},
    };
    photobridge::LinuxFileOps file_ops;

    for (std::size_t iteration = 0; iteration < options.repetitions;
         ++iteration) {
        const auto database_path = options.data_root
            / ("scanner-" + std::to_string(iteration) + ".db");
        RemoveDatabaseFiles(database_path);
        auto connection = Require(
            photobridge::SqliteConnection::Open(database_path),
            "open scanner benchmark database");
        Require(
            photobridge::EnsureSchema(connection),
            "create scanner benchmark schema");
        auto root_fd = Require(
            file_ops.OpenRoot(
                fixture_root,
                photobridge::OpenRootMode::kExisting),
            "open scanner fixture root");

        result.samples.push_back(Measure([&]() {
            photobridge::SqliteManifestBuilder builder(connection, 256);
            Require(
                builder.Begin({
                    "benchmark-scanner",
                    "local-folder",
                    fixture_root.string(),
                }),
                "begin scanner manifest");
            ScanSink sink(builder);
            photobridge::LinuxDirectoryWalker walker;
            Require(
                walker.Walk(root_fd.get(), sink),
                "walk scanner fixture");
            if (sink.asset_count != options.scanner_files) {
                Fail(
                    "scanner fixture asset count mismatch: expected "
                    + std::to_string(options.scanner_files)
                    + ", got " + std::to_string(sink.asset_count));
            }
            const auto frozen = Require(
                builder.Freeze(),
                "freeze scanner manifest");
            if (frozen.asset_count != options.scanner_files) {
                Fail(
                    "scanner manifest asset count mismatch: expected "
                    + std::to_string(options.scanner_files)
                    + ", got " + std::to_string(frozen.asset_count));
            }
            return RunValues{frozen.asset_count, 0};
        }));
        connection = photobridge::SqliteConnection();
        RemoveDatabaseFiles(database_path);
    }
    return result;
}

enum class CopyMode { kSync, kIoUring };

WorkloadResult RunCopy(
    const Options& options,
    const std::filesystem::path& fixture_root,
    CopyMode mode = CopyMode::kSync,
    std::size_t queue_depth = 4)
{
    WorkloadResult result{
        mode == CopyMode::kSync
            ? "copy_hash_fdatasync"
            : "copy_uring_qd" + std::to_string(queue_depth) + "_hash_fdatasync",
        "bytes/s",
        0,
        options.copy_bytes,
        {},
        {},
        {},
    };
    const auto source_path = fixture_root / "copy-source.bin";
    WriteDeterministicFile(source_path, options.copy_bytes);
    auto relative_path = Require(
        photobridge::RelativePath::Parse("copy-source.bin"),
        "parse copy source path");
    photobridge::LinuxFileOps file_ops;
    auto root_fd = Require(
        file_ops.OpenRoot(
            fixture_root,
            photobridge::OpenRootMode::kExisting),
        "open copy benchmark root");
    std::vector<std::byte> buffer(1U * 1024U * 1024U);

    for (std::size_t iteration = 0; iteration < options.repetitions;
         ++iteration) {
        const std::string temp_name =
            ".photobridge-bench-" + std::to_string(iteration) + ".tmp";
        result.samples.push_back(Measure([&]() {
            auto source_fd = Require(
                file_ops.OpenSource(root_fd.get(), relative_path),
                "open copy benchmark source");
            auto target_fd = Require(
                file_ops.CreateTempNoReplace(root_fd.get(), temp_name, 0600),
                "create copy benchmark target");
            photobridge::Blake3Hasher hasher;
            photobridge::CopyResult copied;
            if (mode == CopyMode::kSync) {
                copied = Require(photobridge::CopyAndHash(
                    file_ops,
                    hasher,
                    source_fd.get(),
                    target_fd.get(),
                    buffer), "copy and hash benchmark payload");
            } else {
                auto accelerated = Require(
                    photobridge::TryIoUringCopyAndHash(
                        hasher, source_fd.get(), target_fd.get(),
                        options.copy_bytes, buffer.size(), queue_depth),
                    "io_uring copy benchmark payload");
                if (!accelerated.has_value()) {
                    Fail("io_uring is unavailable; refusing to label a sync copy as io_uring");
                }
                copied = accelerated.value();
            }
            Require(
                file_ops.Fdatasync(target_fd.get()),
                "fdatasync copy benchmark target");
            if (copied.bytes_copied != options.copy_bytes) {
                Fail("copy benchmark byte count mismatch");
            }
            return RunValues{1, copied.bytes_copied};
        }));
        Require(
            file_ops.UnlinkAt(root_fd.get(), temp_name),
            "remove copy benchmark target");
        Require(
            file_ops.FsyncDirectory(root_fd.get()),
            "sync copy benchmark directory");
    }
    return result;
}

enum class SqliteMode { kExecAutocommit, kPreparedAutocommit, kPreparedBatch };

// Match ManifestBuilder's default batch size for the V2 SQLite workload.
constexpr std::size_t kSqliteV2BatchSize = 256;

WorkloadResult RunSqlite(
    const Options& options,
    SqliteMode mode,
    std::size_t batch_size = 1,
    std::string_view report_name = {})
{
    std::string name = "sqlite_batch_insert"; // V1 baseline report name
    if (mode == SqliteMode::kPreparedAutocommit) {
        name = "sqlite_prepared_autocommit";
    } else if (mode == SqliteMode::kPreparedBatch) {
        name = "sqlite_prepared_batch_" + std::to_string(batch_size);
    }
    if (!report_name.empty()) name = report_name;
    WorkloadResult result{
        name,
        "commands/s",
        options.sqlite_commands,
        0,
        {},
        {},
        {},
    };

    for (std::size_t iteration = 0; iteration < options.repetitions;
         ++iteration) {
        const auto database_path = options.data_root
            / ("sqlite-" + std::to_string(iteration) + ".db");
        RemoveDatabaseFiles(database_path);
        auto connection = Require(
            photobridge::SqliteConnection::Open(database_path),
            "open SQLite benchmark database");
        Require(
            photobridge::EnsureSchema(connection),
            "create SQLite benchmark schema");
        Require(
            connection.Execute(
                "CREATE TABLE benchmark_queue(value INTEGER NOT NULL);"),
            "create SQLite benchmark table");

        {
            photobridge::SqliteStatement insert;
            if (mode != SqliteMode::kExecAutocommit
                && insert.Prepare(
                       connection.native_handle(),
                       "INSERT INTO benchmark_queue(value) VALUES(?1);") != SQLITE_OK) {
                Fail("prepare SQLite benchmark insert: "
                    + std::string(sqlite3_errmsg(connection.native_handle())));
            }

            result.samples.push_back(Measure([&]() {
                for (std::size_t index = 0; index < options.sqlite_commands;
                     ++index) {
                    if (mode == SqliteMode::kPreparedBatch
                        && index % batch_size == 0) {
                        Require(connection.Execute("BEGIN IMMEDIATE;"),
                            "begin SQLite benchmark batch");
                    }
                    if (mode == SqliteMode::kExecAutocommit) {
                        Require(connection.Execute(
                            "INSERT INTO benchmark_queue(value) VALUES(1);"),
                            "SQLite benchmark command");
                    } else {
                        if (insert.Reset() != SQLITE_OK
                            || sqlite3_bind_int(insert.get(), 1, 1) != SQLITE_OK
                            || sqlite3_step(insert.get()) != SQLITE_DONE) {
                            Fail("execute prepared SQLite benchmark insert: "
                                + std::string(sqlite3_errmsg(connection.native_handle())));
                        }
                    }
                    if (mode == SqliteMode::kPreparedBatch
                        && ((index + 1) % batch_size == 0
                            || index + 1 == options.sqlite_commands)) {
                        Require(connection.Execute("COMMIT;"),
                            "commit SQLite benchmark batch");
                    }
                }
                return RunValues{options.sqlite_commands, 0};
            }));

            photobridge::SqliteStatement count(
                connection.native_handle(), "SELECT COUNT(*) FROM benchmark_queue;");
            if (count.result() != SQLITE_OK
                || sqlite3_step(count.get()) != SQLITE_ROW
                || sqlite3_column_int64(count.get(), 0)
                    != static_cast<sqlite3_int64>(options.sqlite_commands)) {
                Fail("SQLite benchmark inserted an unexpected number of rows");
            }
            count.Reset();
        }
        connection = photobridge::SqliteConnection();
        RemoveDatabaseFiles(database_path);
    }
    return result;
}

std::string RunCliCommand(
    std::vector<std::string> arguments,
    std::string_view operation)
{
    std::vector<char*> argv;
    argv.reserve(arguments.size());
    for (std::string& argument : arguments) {
        argv.push_back(argument.data());
    }

    std::ostringstream output;
    std::ostringstream errors;
    const int exit_code = photobridge::RunCli(
        static_cast<int>(argv.size()),
        argv.data(),
        output,
        errors);
    if (exit_code != 0) {
        Fail(
            std::string(operation) + " failed with exit code "
            + std::to_string(exit_code) + ": "
            + output.str() + errors.str());
    }
    return output.str();
}

std::string ManifestIdFromScanOutput(std::string_view output)
{
    constexpr std::string_view kPrefix = "scan completed: ";
    const std::size_t start = output.find(kPrefix);
    if (start == std::string_view::npos) {
        Fail("scan output does not contain a manifest id");
    }
    const std::size_t id_start = start + kPrefix.size();
    const std::size_t id_end = output.find(' ', id_start);
    if (id_end == std::string_view::npos || id_end == id_start) {
        Fail("scan output contains a malformed manifest id");
    }
    return std::string(output.substr(id_start, id_end - id_start));
}

std::filesystem::path FindPlanArtifact(
    const std::filesystem::path& workspace)
{
    std::filesystem::path plan_path;
    for (const auto& entry : std::filesystem::directory_iterator(
             workspace / "plans")) {
        if (entry.path().extension() != ".jsonl") continue;
        if (!plan_path.empty()) {
            Fail("end-to-end workspace contains multiple plan artifacts");
        }
        plan_path = entry.path();
    }
    if (plan_path.empty()) {
        Fail("end-to-end workspace contains no plan artifact");
    }
    return plan_path;
}

void CreateE2eFixture(
    const std::filesystem::path& source_root,
    std::size_t file_count,
    std::size_t file_bytes)
{
    std::error_code error;
    std::filesystem::create_directories(source_root, error);
    if (error) Fail("create end-to-end source directory failed");
    for (std::size_t index = 0; index < file_count; ++index) {
        WriteDeterministicFile(
            source_root / ("photo-" + std::to_string(index) + ".jpg"),
            file_bytes);
    }
}

WorkloadResult RunE2e(
    const Options& options,
    const std::filesystem::path& run_root,
    std::size_t workers,
    std::string report_name = {},
    bool report_bytes_per_second = false,
    std::string implementation = {})
{
    if (options.e2e_files > std::numeric_limits<std::size_t>::max()
            / options.e2e_file_bytes) {
        Fail("end-to-end payload size overflows size_t");
    }
    const std::size_t total_bytes =
        options.e2e_files * options.e2e_file_bytes;
    const auto source_root = run_root / "e2e-source";
    if (!std::filesystem::exists(source_root)) {
        CreateE2eFixture(
            source_root,
            options.e2e_files,
            options.e2e_file_bytes);
    }

    WorkloadResult result{
        report_name.empty()
            ? (options.workload == "e2e-large"
                ? "e2e_large_local_pipeline"
                : "e2e_local_pipeline")
            : std::move(report_name),
        report_bytes_per_second ? "bytes/s" : "pipelines/s",
        1,
        total_bytes,
        {},
        {},
        {},
    };
    result.e2e_workers = workers;
    if (!implementation.empty()) {
        result.implementation = std::move(implementation);
    }
    const std::size_t total_iterations =
        options.warmup_repetitions + options.repetitions;
    for (std::size_t iteration = 0; iteration < total_iterations; ++iteration) {
        const bool warmup = iteration < options.warmup_repetitions;
        const auto workspace = run_root
            / ("e2e-workspace-" + std::to_string(workers) + "w-"
               + (warmup ? "warmup-" : "sample-")
               + std::to_string(iteration));
        const auto target = run_root
            / ("e2e-target-" + std::to_string(workers) + "w-"
               + (warmup ? "warmup-" : "sample-")
               + std::to_string(iteration));
        RunCliCommand(
            {"photobridge", "init", "--workspace", workspace.string()},
            "e2e init");

        Sample sample = Measure([&]() {
            const std::string scan_output = RunCliCommand(
                {"photobridge", "scan",
                    "--workspace", workspace.string(),
                    "--source", source_root.string()},
                "e2e scan");
            const std::string manifest_id =
                ManifestIdFromScanOutput(scan_output);
            RunCliCommand(
                {"photobridge", "plan",
                    "--workspace", workspace.string(),
                    "--manifest", manifest_id,
                    "--target", target.string()},
                "e2e plan");
            const std::filesystem::path plan_path =
                FindPlanArtifact(workspace);
            RunCliCommand(
                {"photobridge", "migrate",
                    "--workspace", workspace.string(),
                    "--plan", plan_path.string(),
                    "--workers", std::to_string(workers)},
                "e2e migrate");
            RunCliCommand(
                {"photobridge", "resume",
                    "--workspace", workspace.string(),
                    "--plan", plan_path.string()},
                "e2e resume");
            const std::string verify_output = RunCliCommand(
                {"photobridge", "verify",
                    "--workspace", workspace.string(),
                    "--plan", plan_path.string()},
                "e2e verify");
            if (verify_output.find("status=IDENTICAL")
                == std::string::npos) {
                Fail("e2e verify did not report IDENTICAL");
            }
            return RunValues{1, total_bytes};
        });
        if (!warmup) result.samples.push_back(std::move(sample));

        std::error_code error;
        std::filesystem::remove_all(workspace, error);
        std::filesystem::remove_all(target, error);
        if (error) Fail("clean end-to-end iteration failed");
    }
    return result;
}

double Percentile(std::vector<double> values, double percentile)
{
    std::sort(values.begin(), values.end());
    const auto rank = static_cast<std::size_t>(std::ceil(
        percentile * static_cast<double>(values.size())));
    const std::size_t index = std::min(
        values.size() - 1,
        rank == 0 ? std::size_t{0} : rank - 1);
    return values[index];
}

Json IoJson(const IoCounters& io)
{
    return Json{
        {"rchar_bytes", io.rchar},
        {"wchar_bytes", io.wchar},
        {"read_bytes", io.read_bytes},
        {"write_bytes", io.write_bytes},
        {"read_syscalls", io.syscr},
        {"write_syscalls", io.syscw},
    };
}

Json SampleJson(const Sample& sample)
{
    return Json{
        {"wall_ms", sample.wall_ms},
        {"cpu_ms", sample.cpu_ms},
        {"cpu_utilization_percent",
            sample.wall_ms > 0.0 ? sample.cpu_ms / sample.wall_ms * 100.0
                                : 0.0},
        {"max_rss_kib", sample.max_rss_kib},
        {"rss_delta_kib", sample.rss_delta_kib},
        {"io", IoJson(sample.io)},
        {"items", sample.items},
        {"bytes", sample.bytes},
    };
}

Json WorkloadJson(const WorkloadResult& result)
{
    std::vector<double> wall_ms;
    std::vector<double> cpu_ms;
    std::vector<double> cpu_utilization;
    std::vector<double> throughput;
    wall_ms.reserve(result.samples.size());
    cpu_ms.reserve(result.samples.size());
    cpu_utilization.reserve(result.samples.size());
    throughput.reserve(result.samples.size());
    std::uint64_t peak_rss_kib = 0;
    std::uint64_t total_read_bytes = 0;
    std::uint64_t total_write_bytes = 0;
    std::uint64_t total_read_syscalls = 0;
    std::uint64_t total_write_syscalls = 0;
    for (const Sample& sample : result.samples) {
        wall_ms.push_back(sample.wall_ms);
        cpu_ms.push_back(sample.cpu_ms);
        cpu_utilization.push_back(
            sample.wall_ms > 0.0 ? sample.cpu_ms / sample.wall_ms * 100.0
                                 : 0.0);
        const double numerator = result.unit == "bytes/s"
            ? static_cast<double>(sample.bytes)
            : static_cast<double>(sample.items);
        throughput.push_back(
            sample.wall_ms > 0.0 ? numerator / (sample.wall_ms / 1000.0)
                                 : 0.0);
        peak_rss_kib = std::max(peak_rss_kib, sample.max_rss_kib);
        total_read_bytes += sample.io.read_bytes;
        total_write_bytes += sample.io.write_bytes;
        total_read_syscalls += sample.io.syscr;
        total_write_syscalls += sample.io.syscw;
    }
    auto average = [](const std::vector<double>& values) {
        double total = 0.0;
        for (const double value : values) total += value;
        return values.empty() ? 0.0 : total / values.size();
    };
    const auto min_value = [](const std::vector<double>& values) {
        return *std::min_element(values.begin(), values.end());
    };
    const auto max_value = [](const std::vector<double>& values) {
        return *std::max_element(values.begin(), values.end());
    };

    Json samples = Json::array();
    for (const Sample& sample : result.samples) samples.push_back(SampleJson(sample));
    Json report = Json{
        {"name", result.name},
        {"unit", result.unit},
        {"configured_items", result.configured_items},
        {"configured_bytes", result.configured_bytes},
        {"samples", samples},
        {"summary", {
            {"wall_ms", {
                {"min", min_value(wall_ms)},
                {"mean", average(wall_ms)},
                {"median", Percentile(wall_ms, 0.50)},
                {"p95", Percentile(wall_ms, 0.95)},
                {"p99", Percentile(wall_ms, 0.99)},
                {"max", max_value(wall_ms)},
            }},
            {"cpu_ms", {
                {"mean", average(cpu_ms)},
                {"p95", Percentile(cpu_ms, 0.95)},
                {"p99", Percentile(cpu_ms, 0.99)},
            }},
            {"cpu_utilization_percent", {
                {"mean", average(cpu_utilization)},
                {"p95", Percentile(cpu_utilization, 0.95)},
                {"p99", Percentile(cpu_utilization, 0.99)},
            }},
            {"throughput_per_second", {
                {"min", min_value(throughput)},
                {"mean", average(throughput)},
                {"median", Percentile(throughput, 0.50)},
                {"p95", Percentile(throughput, 0.95)},
                {"p99", Percentile(throughput, 0.99)},
                {"max", max_value(throughput),
                },
            }},
            {"peak_rss_kib", peak_rss_kib},
            {"total_kernel_read_bytes", total_read_bytes},
            {"total_kernel_write_bytes", total_write_bytes},
            {"total_read_syscalls", total_read_syscalls},
            {"total_write_syscalls", total_write_syscalls},
        }},
    };
    if (result.e2e_workers.has_value()) {
        report["e2e_workers"] = *result.e2e_workers;
    }
    if (result.implementation.has_value()) {
        report["implementation"] = *result.implementation;
    }
    return report;
}

void PrintHumanReport(const Json& report)
{
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "PhotoBridge Release benchmark\n"
              << "repetitions=" << report.at("repetitions")
              << ", warmups=" << report.at("warmup_repetitions")
              << ", data_root=" << report.at("data_root")
              << ", e2e_workers=" << report.at("e2e_workers") << "\n";
    for (const auto& workload : report.at("workloads")) {
        const auto& summary = workload.at("summary");
        std::cout << "\n[" << workload.at("name") << "]\n";
        if (workload.contains("implementation")) {
            std::cout << "  implementation=" << workload.at("implementation");
            if (workload.contains("e2e_workers")) {
                std::cout << ", workers=" << workload.at("e2e_workers");
            }
            std::cout << "\n";
        }
        std::cout << "  wall ms: min=" << summary.at("wall_ms").at("min")
                  << " mean=" << summary.at("wall_ms").at("mean")
                  << " p95=" << summary.at("wall_ms").at("p95")
                  << " p99=" << summary.at("wall_ms").at("p99")
                  << " max=" << summary.at("wall_ms").at("max") << "\n"
                  << "  throughput (" << workload.at("unit") << "): mean="
                  << summary.at("throughput_per_second").at("mean")
                  << " p95=" << summary.at("throughput_per_second").at("p95")
                  << " p99=" << summary.at("throughput_per_second").at("p99")
                  << "\n"
                  << "  CPU ms: mean=" << summary.at("cpu_ms").at("mean")
                  << ", CPU utilization mean="
                  << summary.at("cpu_utilization_percent").at("mean")
                  << "%\n"
                  << "  peak RSS KiB=" << summary.at("peak_rss_kib")
                  << ", kernel read bytes="
                  << summary.at("total_kernel_read_bytes")
                  << ", kernel write bytes="
                  << summary.at("total_kernel_write_bytes")
                  << ", read/write syscalls="
                  << summary.at("total_read_syscalls") << "/"
                  << summary.at("total_write_syscalls") << "\n";
    }
}

}  // namespace

int main(int argc, char** argv)
{
    try {
        Options options = ParseOptions(argc, argv);
        if (options.workload == "e2e-large"
            || options.workload == "e2e-v6a"
            || options.workload == "e2e-v6a-matrix") {
            options.e2e_files = 64;
            options.e2e_file_bytes = 16U * 1024U * 1024U;
        }
        const auto run_root = options.data_root / "run";
        const auto scanner_root = run_root / "scanner-fixture";
        std::error_code error;
        std::filesystem::remove_all(run_root, error);
        std::filesystem::create_directories(run_root, error);
        if (error) Fail("create benchmark run directory failed");
        if (options.workload == "all" || options.workload == "scanner") {
            CreateScannerFixture(scanner_root, options.scanner_files);
        }

        Json report{
            {"schema_version", 2},
            {"repetitions", options.repetitions},
            {"warmup_repetitions", options.warmup_repetitions},
            {"data_root", options.data_root.string()},
            {"scanner_files", options.scanner_files},
            {"copy_bytes", options.copy_bytes},
            {"copy_uring_qd", options.copy_uring_qd},
            {"sqlite_commands", options.sqlite_commands},
            {"e2e_files", options.e2e_files},
            {"e2e_file_bytes", options.e2e_file_bytes},
            {"e2e_workers", options.e2e_workers},
            {"workloads", Json::array()},
        };
        if (options.workload == "all" || options.workload == "scanner") {
            report["workloads"].push_back(
                WorkloadJson(RunScanner(options, scanner_root)));
        }
        if (options.workload == "all" || options.workload == "copy"
            || options.workload == "copy-sync"
            || options.workload == "copy-compare") {
            report["workloads"].push_back(
                WorkloadJson(RunCopy(options, run_root)));
        }
        if (options.workload == "copy-uring") {
            report["workloads"].push_back(WorkloadJson(RunCopy(
                options, run_root, CopyMode::kIoUring, options.copy_uring_qd)));
        }
        for (const std::size_t depth : {1U, 2U, 4U, 8U, 16U}) {
            if (options.workload == "copy-compare"
                || options.workload == "copy-uring-qd" + std::to_string(depth)) {
                report["workloads"].push_back(WorkloadJson(RunCopy(
                    options, run_root, CopyMode::kIoUring, depth)));
            }
        }
        if (options.workload == "all" || options.workload == "sqlite"
            || options.workload == "sqlite-v2") {
            report["workloads"].push_back(
                WorkloadJson(RunSqlite(options, SqliteMode::kPreparedBatch,
                    kSqliteV2BatchSize, "sqlite_v2_prepared_batch_256")));
        }
        if (options.workload == "sqlite-v1") {
            report["workloads"].push_back(
                WorkloadJson(RunSqlite(options, SqliteMode::kExecAutocommit,
                    1, "sqlite_v1_exec_autocommit")));
        }
        if (options.workload == "sqlite-compare") {
            report["workloads"].push_back(
                WorkloadJson(RunSqlite(options, SqliteMode::kExecAutocommit)));
            report["workloads"].push_back(
                WorkloadJson(RunSqlite(options, SqliteMode::kPreparedAutocommit)));
            for (const std::size_t batch_size : {10U, 100U, 1000U, 5000U}) {
                report["workloads"].push_back(WorkloadJson(
                    RunSqlite(options, SqliteMode::kPreparedBatch, batch_size)));
            }
        }
        if (options.workload == "sqlite-prepared") {
            report["workloads"].push_back(
                WorkloadJson(RunSqlite(options, SqliteMode::kPreparedAutocommit)));
        }
        for (const std::size_t batch_size : {10U, 100U, 1000U, 5000U}) {
            if (options.workload == "sqlite-batch-" + std::to_string(batch_size)) {
                report["workloads"].push_back(WorkloadJson(
                    RunSqlite(options, SqliteMode::kPreparedBatch, batch_size)));
            }
        }
        if (options.workload == "e2e" || options.workload == "e2e-large") {
            report["workloads"].push_back(
                WorkloadJson(RunE2e(
                    options, run_root, options.e2e_workers)));
        }
        if (options.workload == "e2e-v6a") {
            report["workloads"].push_back(WorkloadJson(RunE2e(
                options,
                run_root,
                options.e2e_workers,
                "e2e_v6a_dedicated_db_writer_"
                    + std::to_string(options.e2e_workers) + "w",
                true,
                "v6a_dedicated_db_writer")));
        }
        if (options.workload == "e2e-v6a-matrix") {
            for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
                report["workloads"].push_back(WorkloadJson(RunE2e(
                    options,
                    run_root,
                    workers,
                    "e2e_v6a_dedicated_db_writer_"
                        + std::to_string(workers) + "w",
                    true,
                    "v6a_dedicated_db_writer")));
            }
        }

        if (!options.output.empty()) {
            std::ofstream output(options.output);
            if (!output) Fail("open benchmark JSON output failed");
            output << report.dump(2) << '\n';
        }
        PrintHumanReport(report);
        if (!options.keep_data) {
            std::filesystem::remove_all(run_root, error);
            if (error) Fail("clean benchmark run directory failed");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "photobridge_bench: " << error.what() << '\n';
        return 1;
    }
}
