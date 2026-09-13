#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "photobridge/common/digest.h"
#include "photobridge/common/status.h"
#include "photobridge/common/status_or.h"
#include "photobridge/filesystem/file_ops.h"
#include "photobridge/model/file_identity.h"

namespace photobridge {

class Hasher {
public:
    virtual ~Hasher() = default;

    virtual Status Update(std::span<const std::byte> bytes) = 0;
    virtual StatusOr<Digest> Finalize() = 0;
};

class Blake3Hasher final : public Hasher {
public:
    Blake3Hasher();
    ~Blake3Hasher() override;

    Blake3Hasher(const Blake3Hasher&) = delete;
    Blake3Hasher& operator=(const Blake3Hasher&) = delete;

    Status Update(std::span<const std::byte> bytes) override;
    StatusOr<Digest> Finalize() override;

private:
    struct State;
    State* state_;
};

struct CopyResult {
    std::uint64_t bytes_copied = 0;
    Digest source_digest;
    FileIdentity source_before;
    FileIdentity source_after;
};

StatusOr<CopyResult> CopyAndHash(
    FileOps& file_ops,
    Hasher& hasher,
    int source_fd,
    int target_fd,
    std::span<std::byte> buffer);

}  // namespace photobridge
