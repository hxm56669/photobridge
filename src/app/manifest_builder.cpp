#include "photobridge/app/manifest_builder.h"

#include <blake3.h>
#include <sqlite3.h>

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace photobridge {
namespace {

constexpr int kManifestBuilding = 0;
constexpr int kManifestFrozen = 1;
constexpr std::string_view kManifestDigestDomain = "PB_MANIFEST_V1";

Status SqliteError(
    sqlite3* database,
    std::string_view operation)
{
    const char* message = database == nullptr
        ? "unknown SQLite error"
        : sqlite3_errmsg(database);
    const int extended_code = database == nullptr
        ? SQLITE_ERROR
        : sqlite3_extended_errcode(database);
    const StatusCode status_code =
        (extended_code & 0xFF) == SQLITE_CONSTRAINT
        ? StatusCode::kAlreadyExists
        : StatusCode::kIoError;
    return Status(
        status_code,
        std::string(operation) + ": " + message);
}

Status RollbackAndReturn(SqliteConnection& connection, Status status)
{
    connection.Execute("ROLLBACK;");
    return status;
}

Status BindText(
    sqlite3_stmt* statement,
    int index,
    std::string_view value,
    std::string_view field_name)
{
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return Status(
            StatusCode::kInvalidArgument,
            std::string("SQLite text field is too large: ")
                + std::string(field_name));
    }

    const int result = sqlite3_bind_text(
        statement,
        index,
        value.data(),
        static_cast<int>(value.size()),
        SQLITE_TRANSIENT);
    if (result != SQLITE_OK) {
        return Status(
            StatusCode::kIoError,
            std::string("bind SQLite ") + std::string(field_name)
                + ": " + sqlite3_errstr(result));
    }
    return Status::Ok();
}

Status BindBlob(
    sqlite3_stmt* statement,
    int index,
    std::string_view value,
    std::string_view field_name)
{
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return Status(
            StatusCode::kInvalidArgument,
            std::string("SQLite blob field is too large: ")
                + std::string(field_name));
    }

    const int result = sqlite3_bind_blob(
        statement,
        index,
        value.data(),
        static_cast<int>(value.size()),
        SQLITE_TRANSIENT);
    if (result != SQLITE_OK) {
        return Status(
            StatusCode::kIoError,
            std::string("bind SQLite ") + std::string(field_name)
                + ": " + sqlite3_errstr(result));
    }
    return Status::Ok();
}

Status BindUint64(
    sqlite3_stmt* statement,
    int index,
    std::uint64_t value,
    std::string_view field_name)
{
    if (value > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())) {
        return Status(
            StatusCode::kInvalidArgument,
            std::string("SQLite integer field overflows: ")
                + std::string(field_name));
    }

    const int result = sqlite3_bind_int64(
        statement,
        index,
        static_cast<sqlite3_int64>(value));
    if (result != SQLITE_OK) {
        return Status(
            StatusCode::kIoError,
            std::string("bind SQLite ") + std::string(field_name)
                + ": " + sqlite3_errstr(result));
    }
    return Status::Ok();
}

Status BindInt64(
    sqlite3_stmt* statement,
    int index,
    std::int64_t value,
    std::string_view field_name)
{
    const int result = sqlite3_bind_int64(
        statement,
        index,
        static_cast<sqlite3_int64>(value));
    if (result != SQLITE_OK) {
        return Status(
            StatusCode::kIoError,
            std::string("bind SQLite ") + std::string(field_name)
                + ": " + sqlite3_errstr(result));
    }
    return Status::Ok();
}

Status BindRawBlob(
    sqlite3_stmt* statement,
    int index,
    const void* data,
    std::size_t size,
    std::string_view field_name)
{
    if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return Status(
            StatusCode::kInvalidArgument,
            std::string("SQLite blob field is too large: ")
                + std::string(field_name));
    }

    const int result = sqlite3_bind_blob(
        statement,
        index,
        data,
        static_cast<int>(size),
        SQLITE_TRANSIENT);
    if (result != SQLITE_OK) {
        return Status(
            StatusCode::kIoError,
            std::string("bind SQLite ") + std::string(field_name)
                + ": " + sqlite3_errstr(result));
    }
    return Status::Ok();
}

std::string NewManifestId()
{
    static std::atomic<std::uint64_t> sequence = 0;
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch());
    return "manifest-" + std::to_string(now.count()) + "-"
        + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

Status PrepareStatement(
    sqlite3* database,
    const char* sql,
    sqlite3_stmt** statement,
    std::string_view operation)
{
    const int result = sqlite3_prepare_v2(
        database,
        sql,
        -1,
        statement,
        nullptr);
    if (result != SQLITE_OK) {
        return SqliteError(database, operation);
    }
    return Status::Ok();
}

class CanonicalHasher {
public:
    CanonicalHasher()
    {
        blake3_hasher_init(&hasher_);
    }

    Status AppendRaw(
        const void* data,
        std::size_t size)
    {
        blake3_hasher_update(&hasher_, data, size);
        return Status::Ok();
    }

    Status AppendUint64(
        std::uint64_t value,
        std::string_view field_name)
    {
        static_cast<void>(field_name);
        std::array<std::byte, sizeof(value)> encoded{};
        for (std::size_t index = 0; index < encoded.size(); ++index) {
            encoded[index] = static_cast<std::byte>(
                (value >> (index * 8)) & 0xFFU);
        }
        return AppendRaw(encoded.data(), encoded.size());
    }

    Status AppendInt64(
        std::int64_t value,
        std::string_view field_name)
    {
        return AppendUint64(static_cast<std::uint64_t>(value), field_name);
    }

    Status AppendBytes(
        const void* data,
        std::size_t size,
        std::string_view field_name)
    {
        Status status = AppendUint64(
            static_cast<std::uint64_t>(size),
            std::string(field_name) + " length");
        if (!status.ok()) {
            return status;
        }
        return AppendRaw(data, size);
    }

    Digest Finalize()
    {
        Digest digest;
        blake3_hasher_finalize(
            &hasher_,
            reinterpret_cast<std::uint8_t*>(digest.bytes.data()),
            digest.bytes.size());
        return digest;
    }

private:
    blake3_hasher hasher_{};
};

Status AppendTextColumn(
    sqlite3_stmt* statement,
    int index,
    CanonicalHasher& hasher,
    std::string_view field_name)
{
    if (sqlite3_column_type(statement, index) != SQLITE_TEXT) {
        return Status(
            StatusCode::kInternal,
            std::string("SQLite canonical field is not text: ")
                + std::string(field_name));
    }
    const unsigned char* value = sqlite3_column_text(statement, index);
    const int size = sqlite3_column_bytes(statement, index);
    if (value == nullptr || size < 0) {
        return Status(
            StatusCode::kInternal,
            std::string("SQLite canonical text field is invalid: ")
                + std::string(field_name));
    }
    return hasher.AppendBytes(value, static_cast<std::size_t>(size), field_name);
}

Status AppendBlobColumn(
    sqlite3_stmt* statement,
    int index,
    CanonicalHasher& hasher,
    std::string_view field_name)
{
    if (sqlite3_column_type(statement, index) != SQLITE_BLOB) {
        return Status(
            StatusCode::kInternal,
            std::string("SQLite canonical field is not a blob: ")
                + std::string(field_name));
    }
    const void* value = sqlite3_column_blob(statement, index);
    const int size = sqlite3_column_bytes(statement, index);
    if (size < 0 || (value == nullptr && size != 0)) {
        return Status(
            StatusCode::kInternal,
            std::string("SQLite canonical blob field is invalid: ")
                + std::string(field_name));
    }
    return hasher.AppendBytes(value, static_cast<std::size_t>(size), field_name);
}

Status AppendIntegerColumn(
    sqlite3_stmt* statement,
    int index,
    CanonicalHasher& hasher,
    std::string_view field_name)
{
    if (sqlite3_column_type(statement, index) != SQLITE_INTEGER) {
        return Status(
            StatusCode::kInternal,
            std::string("SQLite canonical field is not an integer: ")
                + std::string(field_name));
    }
    return hasher.AppendInt64(
        static_cast<std::int64_t>(sqlite3_column_int64(statement, index)),
        field_name);
}

Status AppendNonnegativeIntegerColumn(
    sqlite3_stmt* statement,
    int index,
    CanonicalHasher& hasher,
    std::string_view field_name)
{
    if (sqlite3_column_type(statement, index) != SQLITE_INTEGER) {
        return Status(
            StatusCode::kInternal,
            std::string("SQLite canonical field is not an integer: ")
                + std::string(field_name));
    }
    const sqlite3_int64 value = sqlite3_column_int64(statement, index);
    if (value < 0) {
        return Status(
            StatusCode::kInternal,
            std::string("SQLite canonical field is negative: ")
                + std::string(field_name));
    }
    return hasher.AppendUint64(
        static_cast<std::uint64_t>(value),
        field_name);
}

StatusOr<Digest> ComputeManifestDigest(
    sqlite3* database,
    std::string_view manifest_id)
{
    CanonicalHasher hasher;
    Status status = hasher.AppendBytes(
        kManifestDigestDomain.data(),
        kManifestDigestDomain.size(),
        "manifest domain");
    if (!status.ok()) {
        return status;
    }

    sqlite3_stmt* source_statement = nullptr;
    status = PrepareStatement(
        database,
        "SELECT source_id, source_type, source_root "
        "FROM source_manifest WHERE manifest_id = ?;",
        &source_statement,
        "prepare manifest digest source query");
    if (!status.ok()) {
        return status;
    }
    status = BindText(
        source_statement,
        1,
        manifest_id,
        "manifest_id");
    if (!status.ok()) {
        sqlite3_finalize(source_statement);
        return status;
    }
    if (sqlite3_step(source_statement) != SQLITE_ROW) {
        status = SqliteError(database, "read manifest digest source");
        sqlite3_finalize(source_statement);
        return status;
    }

    status = AppendTextColumn(
        source_statement,
        0,
        hasher,
        "source_id");
    if (status.ok()) {
        status = AppendTextColumn(
            source_statement,
            1,
            hasher,
            "source_type");
    }
    if (status.ok()) {
        status = AppendBlobColumn(
            source_statement,
            2,
            hasher,
            "source_root");
    }
    const int source_final_step = sqlite3_step(source_statement);
    if (status.ok() && source_final_step != SQLITE_DONE) {
        status = SqliteError(database, "read manifest digest source");
    }
    sqlite3_finalize(source_statement);
    if (!status.ok()) {
        return status;
    }

    sqlite3_stmt* asset_statement = nullptr;
    status = PrepareStatement(
        database,
        "SELECT relative_path, device, inode, size, mtime_ns, ctime_ns, kind "
        "FROM physical_asset WHERE manifest_id = ? "
        "ORDER BY relative_path ASC, asset_id ASC;",
        &asset_statement,
        "prepare manifest digest asset query");
    if (!status.ok()) {
        return status;
    }
    status = BindText(
        asset_statement,
        1,
        manifest_id,
        "manifest_id");
    if (!status.ok()) {
        sqlite3_finalize(asset_statement);
        return status;
    }

    while (true) {
        const int step_result = sqlite3_step(asset_statement);
        if (step_result == SQLITE_DONE) {
            break;
        }
        if (step_result != SQLITE_ROW) {
            status = SqliteError(database, "read manifest digest asset");
            break;
        }

        status = AppendBlobColumn(
            asset_statement,
            0,
            hasher,
            "relative_path");
        if (status.ok()) {
            status = AppendNonnegativeIntegerColumn(
                asset_statement,
                1,
                hasher,
                "device");
        }
        if (status.ok()) {
            status = AppendNonnegativeIntegerColumn(
                asset_statement,
                2,
                hasher,
                "inode");
        }
        if (status.ok()) {
            status = AppendNonnegativeIntegerColumn(
                asset_statement,
                3,
                hasher,
                "size");
        }
        if (status.ok()) {
            status = AppendIntegerColumn(
                asset_statement,
                4,
                hasher,
                "mtime_ns");
        }
        if (status.ok()) {
            status = AppendIntegerColumn(
                asset_statement,
                5,
                hasher,
                "ctime_ns");
        }
        if (status.ok()) {
            status = AppendIntegerColumn(
                asset_statement,
                6,
                hasher,
                "kind");
        }
        if (!status.ok()) {
            break;
        }
    }
    sqlite3_finalize(asset_statement);
    if (!status.ok()) {
        return status;
    }
    return hasher.Finalize();
}

StatusOr<FrozenManifest> ReadFrozenManifest(
    SqliteConnection& connection,
    std::string_view manifest_id)
{
    Status status = connection.Execute("BEGIN;");
    if (!status.ok()) {
        return status;
    }

    sqlite3_stmt* statement = nullptr;
    status = PrepareStatement(
        connection.native_handle(),
        "SELECT source_manifest.state, source_manifest.manifest_digest, "
        "COUNT(physical_asset.asset_id) "
        "FROM source_manifest "
        "LEFT JOIN physical_asset ON physical_asset.manifest_id "
        "= source_manifest.manifest_id "
        "WHERE source_manifest.manifest_id = ? "
        "GROUP BY source_manifest.manifest_id, source_manifest.state, "
        "source_manifest.manifest_digest;",
        &statement,
        "prepare frozen manifest query");
    if (!status.ok()) {
        return RollbackAndReturn(connection, status);
    }

    status = BindText(statement, 1, manifest_id, "manifest_id");
    if (!status.ok()) {
        sqlite3_finalize(statement);
        return RollbackAndReturn(connection, status);
    }

    const int step_result = sqlite3_step(statement);
    if (step_result == SQLITE_DONE) {
        sqlite3_finalize(statement);
        return RollbackAndReturn(
            connection,
            Status(
                StatusCode::kNotFound,
                "manifest was not found"));
    }
    if (step_result != SQLITE_ROW) {
        status = SqliteError(connection.native_handle(), "read frozen manifest");
        sqlite3_finalize(statement);
        return RollbackAndReturn(connection, status);
    }

    if (sqlite3_column_type(statement, 0) != SQLITE_INTEGER) {
        sqlite3_finalize(statement);
        return RollbackAndReturn(
            connection,
            Status(
                StatusCode::kInternal,
                "frozen manifest state is not an integer"));
    }
    const sqlite3_int64 persisted_state = sqlite3_column_int64(statement, 0);
    if (persisted_state != kManifestFrozen) {
        sqlite3_finalize(statement);
        return RollbackAndReturn(
            connection,
            Status(
                StatusCode::kInvalidArgument,
                "manifest is not frozen"));
    }

    if (sqlite3_column_type(statement, 1) != SQLITE_BLOB
        || sqlite3_column_bytes(statement, 1)
            != static_cast<int>(Digest{}.bytes.size())) {
        sqlite3_finalize(statement);
        return RollbackAndReturn(
            connection,
            Status(
                StatusCode::kInternal,
                "frozen manifest digest is missing or malformed"));
    }
    Digest persisted_digest;
    std::memcpy(
        persisted_digest.bytes.data(),
        sqlite3_column_blob(statement, 1),
        persisted_digest.bytes.size());

    if (sqlite3_column_type(statement, 2) != SQLITE_INTEGER) {
        sqlite3_finalize(statement);
        return RollbackAndReturn(
            connection,
            Status(
                StatusCode::kInternal,
                "frozen manifest asset count is not an integer"));
    }
    const sqlite3_int64 persisted_count = sqlite3_column_int64(statement, 2);
    if (persisted_count < 0) {
        sqlite3_finalize(statement);
        return RollbackAndReturn(
            connection,
            Status(
                StatusCode::kInternal,
                "frozen manifest asset count is negative"));
    }

    if (sqlite3_step(statement) != SQLITE_DONE) {
        status = SqliteError(
            connection.native_handle(),
            "read frozen manifest");
        sqlite3_finalize(statement);
        return RollbackAndReturn(connection, status);
    }
    sqlite3_finalize(statement);

    auto computed_digest = ComputeManifestDigest(
        connection.native_handle(),
        manifest_id);
    if (!computed_digest.ok()) {
        return RollbackAndReturn(connection, computed_digest.status());
    }
    if (computed_digest.value() != persisted_digest) {
        return RollbackAndReturn(
            connection,
            Status(
                StatusCode::kInternal,
                "frozen manifest digest does not match its assets"));
    }

    status = connection.Execute("COMMIT;");
    if (!status.ok()) {
        connection.Execute("ROLLBACK;");
        return status;
    }

    return FrozenManifest{
        std::string(manifest_id),
        static_cast<std::uint64_t>(persisted_count),
        persisted_digest,
    };
}

}  // namespace

SqliteManifestBuilder::SqliteManifestBuilder(
    SqliteConnection& connection,
    std::size_t batch_size)
    : connection_(connection),
      batch_size_(batch_size == 0 ? 1 : batch_size) {}

StatusOr<SqliteManifestBuilder> SqliteManifestBuilder::Reopen(
    SqliteConnection& connection,
    std::string manifest_id)
{
    if (manifest_id.empty()) {
        return Status(
            StatusCode::kInvalidArgument,
            "manifest id must not be empty");
    }

    auto frozen = ReadFrozenManifest(connection, manifest_id);
    if (!frozen.ok()) {
        return frozen.status();
    }

    SqliteManifestBuilder builder(connection);
    builder.state_ = ManifestBuilderState::kFrozen;
    builder.manifest_id_ = std::move(frozen.value().manifest_id);
    builder.asset_count_ = frozen.value().asset_count;
    builder.manifest_digest_ = frozen.value().manifest_digest;
    return builder;
}

Status SqliteManifestBuilder::Begin(SourceDescriptor source)
{
    if (state_ != ManifestBuilderState::kIdle) {
        return Status(
            StatusCode::kInvalidArgument,
            "manifest builder has already been started");
    }
    if (source.source_id.empty()
        || source.source_type.empty()
        || source.source_root.empty()) {
        return Status(
            StatusCode::kInvalidArgument,
            "manifest source fields must not be empty");
    }

    const std::string candidate_manifest_id = NewManifestId();
    Status status = connection_.Execute("BEGIN IMMEDIATE;");
    if (!status.ok()) {
        return status;
    }

    sqlite3_stmt* statement = nullptr;
    status = PrepareStatement(
        connection_.native_handle(),
        "INSERT INTO source_manifest("
        "manifest_id, source_id, source_type, source_root, state, "
        "manifest_digest, created_at_ns) VALUES (?, ?, ?, ?, ?, NULL, ?);",
        &statement,
        "prepare source manifest insert");
    if (!status.ok()) {
        return RollbackAndReturn(connection_, status);
    }

    status = BindText(statement, 1, candidate_manifest_id, "manifest_id");
    if (status.ok()) {
        status = BindText(statement, 2, source.source_id, "source_id");
    }
    if (status.ok()) {
        status = BindText(statement, 3, source.source_type, "source_type");
    }
    if (status.ok()) {
        status = BindBlob(statement, 4, source.source_root, "source_root");
    }
    if (status.ok()) {
        status = sqlite3_bind_int(statement, 5, kManifestBuilding) == SQLITE_OK
            ? Status::Ok()
            : Status(
                StatusCode::kIoError,
                "bind SQLite manifest state failed");
    }
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch());
    if (status.ok()) {
        status = BindInt64(
            statement,
            6,
            now.count(),
            "created_at_ns");
    }
    if (status.ok() && sqlite3_step(statement) != SQLITE_DONE) {
        status = SqliteError(
            connection_.native_handle(),
            "insert source manifest");
    }
    sqlite3_finalize(statement);
    if (!status.ok()) {
        return RollbackAndReturn(connection_, status);
    }

    status = connection_.Execute("COMMIT;");
    if (!status.ok()) {
        connection_.Execute("ROLLBACK;");
        return status;
    }

    manifest_id_ = candidate_manifest_id;
    asset_count_ = 0;
    manifest_digest_ = Digest{};
    pending_assets_.clear();
    state_ = ManifestBuilderState::kBuilding;
    return Status::Ok();
}

Status SqliteManifestBuilder::Add(PhysicalAsset asset)
{
    if (state_ != ManifestBuilderState::kBuilding) {
        return Status(
            StatusCode::kInvalidArgument,
            "manifest builder is not building");
    }
    if (asset.relative_path.bytes().empty()) {
        return Status(
            StatusCode::kInvalidArgument,
            "physical asset relative path must not be empty");
    }

    pending_assets_.push_back(std::move(asset));
    if (pending_assets_.size() < batch_size_) {
        return Status::Ok();
    }
    return FlushPending();
}

Status SqliteManifestBuilder::FlushPending()
{
    if (pending_assets_.empty()) {
        return Status::Ok();
    }

    Status status = connection_.Execute("BEGIN IMMEDIATE;");
    if (!status.ok()) {
        return status;
    }

    sqlite3_stmt* statement = nullptr;
    status = PrepareStatement(
        connection_.native_handle(),
        "INSERT INTO physical_asset("
        "manifest_id, asset_id, relative_path, display_path, device, inode, "
        "size, mtime_ns, ctime_ns, kind) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
        &statement,
        "prepare physical asset insert");
    if (!status.ok()) {
        return RollbackAndReturn(connection_, status);
    }

    std::uint64_t batch_count = 0;
    for (const PhysicalAsset& asset : pending_assets_) {
        const PhysicalAssetId asset_id = PhysicalAssetIdFor(asset);
        status = BindText(statement, 1, manifest_id_, "manifest_id");
        if (status.ok()) {
            status = BindText(statement, 2, asset_id, "asset_id");
        }
        if (status.ok()) {
            status = BindBlob(
                statement,
                3,
                asset.relative_path.bytes(),
                "relative_path");
        }
        const std::string display_path = asset.relative_path.DisplayString();
        if (status.ok()) {
            status = BindText(statement, 4, display_path, "display_path");
        }
        if (status.ok()) {
            status = BindUint64(
                statement,
                5,
                asset.identity.device,
                "device");
        }
        if (status.ok()) {
            status = BindUint64(
                statement,
                6,
                asset.identity.inode,
                "inode");
        }
        if (status.ok()) {
            status = BindUint64(
                statement,
                7,
                asset.identity.size,
                "size");
        }
        if (status.ok()) {
            status = BindInt64(
                statement,
                8,
                asset.identity.mtime_ns,
                "mtime_ns");
        }
        if (status.ok()) {
            status = BindInt64(
                statement,
                9,
                asset.identity.ctime_ns,
                "ctime_ns");
        }
        if (status.ok()) {
            status = sqlite3_bind_int(
                statement,
                10,
                static_cast<int>(asset.kind)) == SQLITE_OK
                ? Status::Ok()
                : Status(
                    StatusCode::kIoError,
                    "bind SQLite asset kind failed");
        }
        if (status.ok() && sqlite3_step(statement) != SQLITE_DONE) {
            status = SqliteError(
                connection_.native_handle(),
                "insert physical asset");
        }
        sqlite3_clear_bindings(statement);
        sqlite3_reset(statement);
        if (!status.ok()) {
            break;
        }
        ++batch_count;
    }
    sqlite3_finalize(statement);
    if (!status.ok()) {
        return RollbackAndReturn(connection_, status);
    }

    status = connection_.Execute("COMMIT;");
    if (!status.ok()) {
        connection_.Execute("ROLLBACK;");
        return status;
    }

    asset_count_ += batch_count;
    pending_assets_.erase(
        pending_assets_.begin(),
        pending_assets_.begin() + static_cast<std::ptrdiff_t>(batch_count));
    return Status::Ok();
}

StatusOr<FrozenManifest> SqliteManifestBuilder::Freeze()
{
    if (state_ != ManifestBuilderState::kBuilding) {
        return Status(
            StatusCode::kInvalidArgument,
            "manifest builder is not building");
    }

    Status status = FlushPending();
    if (!status.ok()) {
        return status;
    }

    status = connection_.Execute("BEGIN IMMEDIATE;");
    if (!status.ok()) {
        return status;
    }
    auto digest = ComputeManifestDigest(
        connection_.native_handle(),
        manifest_id_);
    if (!digest.ok()) {
        return RollbackAndReturn(connection_, digest.status());
    }

    sqlite3_stmt* update_statement = nullptr;
    status = PrepareStatement(
        connection_.native_handle(),
        "UPDATE source_manifest SET state = ?, manifest_digest = ? "
        "WHERE manifest_id = ? AND state = ?;",
        &update_statement,
        "prepare manifest freeze");
    if (!status.ok()) {
        return RollbackAndReturn(connection_, status);
    }

    status = sqlite3_bind_int(
        update_statement,
        1,
        kManifestFrozen) == SQLITE_OK
        ? Status::Ok()
        : Status(StatusCode::kIoError, "bind SQLite manifest state failed");
    if (status.ok()) {
        status = BindRawBlob(
            update_statement,
            2,
            digest.value().bytes.data(),
            digest.value().bytes.size(),
            "manifest_digest");
    }
    if (status.ok()) {
        status = BindText(
            update_statement,
            3,
            manifest_id_,
            "manifest_id");
    }
    if (status.ok()) {
        status = sqlite3_bind_int(
            update_statement,
            4,
            kManifestBuilding) == SQLITE_OK
            ? Status::Ok()
            : Status(
                StatusCode::kIoError,
                "bind SQLite manifest building state failed");
    }
    if (status.ok() && sqlite3_step(update_statement) != SQLITE_DONE) {
        status = SqliteError(
            connection_.native_handle(),
            "freeze source manifest");
    }
    sqlite3_finalize(update_statement);
    if (!status.ok()) {
        return RollbackAndReturn(connection_, status);
    }

    sqlite3_stmt* statement = nullptr;
    status = PrepareStatement(
        connection_.native_handle(),
        "SELECT changes();",
        &statement,
        "read manifest freeze result");
    if (!status.ok()) {
        return RollbackAndReturn(connection_, status);
    }
    const int step_result = sqlite3_step(statement);
    const int changed = step_result == SQLITE_ROW
        ? sqlite3_column_int(statement, 0)
        : 0;
    if (step_result != SQLITE_ROW || changed != 1) {
        sqlite3_finalize(statement);
        return RollbackAndReturn(
            connection_,
            Status(
                StatusCode::kInternal,
                "manifest freeze updated an unexpected number of rows"));
    }
    sqlite3_finalize(statement);

    status = connection_.Execute("COMMIT;");
    if (!status.ok()) {
        connection_.Execute("ROLLBACK;");
        return status;
    }

    state_ = ManifestBuilderState::kFrozen;
    manifest_digest_ = digest.value();
    return FrozenManifest{manifest_id_, asset_count_, digest.value()};
}

StatusOr<FrozenManifest> SqliteManifestBuilder::frozen_manifest() const
{
    if (state_ != ManifestBuilderState::kFrozen) {
        return Status(
            StatusCode::kInvalidArgument,
            "manifest builder does not contain a frozen manifest");
    }
    return FrozenManifest{manifest_id_, asset_count_, manifest_digest_};
}

ManifestBuilderState SqliteManifestBuilder::state() const noexcept
{
    return state_;
}

}  // namespace photobridge
