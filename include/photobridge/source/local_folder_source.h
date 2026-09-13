#pragma once

#include <filesystem>

#include "photobridge/source/source_adapter.h"

namespace photobridge {

class LocalFolderSource final : public SourceAdapter {
public:
    explicit LocalFolderSource(std::filesystem::path root);

    Status Scan(PhysicalAssetSink& sink) override;
    std::string_view TypeName() const noexcept override;

private:
    std::filesystem::path root_;
};

}  // namespace photobridge
