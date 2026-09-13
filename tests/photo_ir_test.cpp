#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "photobridge/model/photo_ir.h"

namespace {

photobridge::PhysicalAsset Asset(
    std::string path,
    photobridge::AssetKind kind,
    std::string extension)
{
    auto relative = photobridge::RelativePath::Parse(std::move(path));
    EXPECT_TRUE(relative.ok());
    return photobridge::PhysicalAsset{
        std::move(relative.value()),
        photobridge::FileIdentity{},
        kind,
        std::move(extension),
    };
}

photobridge::AssociationEdge Edge(
    const photobridge::PhysicalAsset& media,
    const photobridge::PhysicalAsset& sidecar,
    photobridge::AssociationStatus status =
        photobridge::AssociationStatus::kConfirmed)
{
    return photobridge::AssociationEdge{
        photobridge::EdgeKind::kMetadataAttachment,
        photobridge::PhysicalAssetIdFor(media),
        photobridge::PhysicalAssetIdFor(sidecar),
        photobridge::RelationType::kJsonSidecar,
        "takeout.basename.v1",
        photobridge::Evidence{"takeout.basename.v1", "photo.json#/title"},
        status,
    };
}

}  // namespace

TEST(PhotoIrTest, ProjectsStableMediaAndConfirmedRelations)
{
    auto media = Asset("DCIM/photo.JPG", photobridge::AssetKind::kMedia, "jpg");
    auto sidecar = Asset(
        "DCIM/photo.json",
        photobridge::AssetKind::kSidecarJson,
        "json");
    const auto media_id = photobridge::PhysicalAssetIdFor(media);
    const auto sidecar_id = photobridge::PhysicalAssetIdFor(sidecar);
    auto logical_id = photobridge::LogicalAssetIdForMembers({media_id, sidecar_id});
    ASSERT_TRUE(logical_id.ok());

    std::vector<photobridge::PhysicalAsset> physical{sidecar, media};
    std::vector<photobridge::AssociationEdge> edges{Edge(media, sidecar)};
    const auto photo = photobridge::BuildCanonicalPhoto(
        photobridge::LogicalAsset{
            logical_id.value(),
            {sidecar_id, media_id},
            std::nullopt,
        },
        physical,
        edges);
    ASSERT_TRUE(photo.ok()) << photo.status().message();
    EXPECT_EQ(photo.value().members, (std::vector<std::string>{media_id, sidecar_id}));
    ASSERT_EQ(photo.value().media_components.size(), 1U);
    EXPECT_EQ(photo.value().media_components[0].asset_id, media_id);
    EXPECT_EQ(photo.value().media_components[0].relative_path.DisplayString(), "DCIM/photo.JPG");
    ASSERT_EQ(photo.value().relationships.size(), 1U);
    EXPECT_EQ(photo.value().relationships[0].evidence.detail, "photo.json#/title");
}

TEST(PhotoIrTest, IgnoresUnconfirmedAndOutOfComponentEdges)
{
    auto media = Asset("photo.jpg", photobridge::AssetKind::kMedia, "jpg");
    auto sidecar = Asset("photo.json", photobridge::AssetKind::kSidecarJson, "json");
    auto other = Asset("other.jpg", photobridge::AssetKind::kMedia, "jpg");
    const auto media_id = photobridge::PhysicalAssetIdFor(media);
    const auto sidecar_id = photobridge::PhysicalAssetIdFor(sidecar);
    const auto other_id = photobridge::PhysicalAssetIdFor(other);
    auto logical_id = photobridge::LogicalAssetIdForMembers({media_id, sidecar_id});
    ASSERT_TRUE(logical_id.ok());
    std::vector<photobridge::PhysicalAsset> physical{media, sidecar, other};
    std::vector<photobridge::AssociationEdge> edges{
        Edge(media, sidecar, photobridge::AssociationStatus::kAmbiguous),
        Edge(media, other),
    };

    const auto photo = photobridge::BuildCanonicalPhoto(
        photobridge::LogicalAsset{logical_id.value(), {media_id, sidecar_id}, std::nullopt},
        physical,
        edges);
    ASSERT_TRUE(photo.ok());
    EXPECT_TRUE(photo.value().relationships.empty());
    EXPECT_NE(other_id, media_id);
}

TEST(PhotoIrTest, RejectsMissingMemberOrUnstableId)
{
    auto media = Asset("photo.jpg", photobridge::AssetKind::kMedia, "jpg");
    const auto media_id = photobridge::PhysicalAssetIdFor(media);
    auto logical_id = photobridge::LogicalAssetIdForMembers({media_id});
    ASSERT_TRUE(logical_id.ok());
    const auto missing = photobridge::BuildCanonicalPhoto(
        photobridge::LogicalAsset{logical_id.value(), {"missing"}, std::nullopt},
        std::vector<photobridge::PhysicalAsset>{media},
        std::vector<photobridge::AssociationEdge>{});
    EXPECT_EQ(missing.status().code(), photobridge::StatusCode::kInvalidArgument);

    const auto unstable = photobridge::BuildCanonicalPhoto(
        photobridge::LogicalAsset{"wrong", {media_id}, std::nullopt},
        std::vector<photobridge::PhysicalAsset>{media},
        std::vector<photobridge::AssociationEdge>{});
    EXPECT_EQ(unstable.status().code(), photobridge::StatusCode::kInvalidArgument);
}
