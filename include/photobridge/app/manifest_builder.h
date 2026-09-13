#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "photobridge/common/status.h"
#include "photobridge/common/status_or.h"
#include "photobridge/app/sqlite_connection.h"
#include "photobridge/common/digest.h"
#include "photobridge/model/physical_asset.h"
#include "photobridge/source/source_adapter.h"

namespace photobridge {

struct SourceDescriptor {
    std::string source_id;
    std::string source_type;
    std::string source_root;
};

enum class ManifestBuilderState {
    kIdle,
    kBuilding,
    kFrozen,
};

struct FrozenManifest {
    std::string manifest_id;
    std::uint64_t asset_count = 0;
    Digest manifest_digest;
};

class ManifestBuilder : public PhysicalAssetSink {
public:
    virtual Status Begin(SourceDescriptor source) = 0;
    virtual StatusOr<FrozenManifest> Freeze() = 0;
    virtual ManifestBuilderState state() const noexcept = 0;
    virtual ~ManifestBuilder() = default;
};

class SqliteManifestBuilder final : public ManifestBuilder {
public:
    explicit SqliteManifestBuilder(
        SqliteConnection& connection,
        std::size_t batch_size = 256);

    static StatusOr<SqliteManifestBuilder> Reopen(
        SqliteConnection& connection,
        std::string manifest_id);

    Status Begin(SourceDescriptor source) override;
    Status Add(PhysicalAsset asset) override;
    StatusOr<FrozenManifest> Freeze() override;
    StatusOr<FrozenManifest> frozen_manifest() const;
    ManifestBuilderState state() const noexcept override;

private:
    Status FlushPending();

    SqliteConnection& connection_;
    std::size_t batch_size_;
    ManifestBuilderState state_ = ManifestBuilderState::kIdle;
    std::string manifest_id_;
    std::uint64_t asset_count_ = 0;
    Digest manifest_digest_;
    std::vector<PhysicalAsset> pending_assets_;
};

}  // namespace photobridge
