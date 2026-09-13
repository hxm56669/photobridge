#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "photobridge/common/digest.h"
#include "photobridge/common/status_or.h"

namespace photobridge {

enum class UploadFileState {
    kPending,
    kReceiving,
    kReceived,
    kFailed,
};

enum class UploadSessionState {
    kOpen,
    kComplete,
    kFailed,
};

enum class BeginUploadResult {
    kStarted,
    kAlreadyReceived,
    kBusy,
};

struct UploadFileRequest {
    std::string original_filename;
    std::uint64_t expected_size = 0;
    std::optional<Digest> client_digest;
};

struct UploadFile {
    std::string file_id;
    std::string original_filename;
    std::string safe_filename;
    std::uint64_t expected_size = 0;
    std::uint64_t received_size = 0;
    std::optional<Digest> client_digest;
    std::optional<Digest> server_digest;
    UploadFileState state = UploadFileState::kPending;
};

struct UploadSession {
    std::string session_id;
    std::int64_t created_at_ns = 0;
    std::uint64_t expected_total_bytes = 0;
    std::uint64_t received_total_bytes = 0;
    UploadSessionState state = UploadSessionState::kOpen;
    std::vector<UploadFile> files;
};

struct UploadQuota {
    std::uint64_t max_session_bytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;
    std::uint64_t max_file_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
    std::uint64_t max_reserved_bytes = 16ULL * 1024ULL * 1024ULL * 1024ULL;
};

class UploadSessionStore final {
public:
    ~UploadSessionStore();

    UploadSessionStore(const UploadSessionStore&) = delete;
    UploadSessionStore& operator=(const UploadSessionStore&) = delete;

    UploadSessionStore(UploadSessionStore&& other) noexcept;
    UploadSessionStore& operator=(UploadSessionStore&& other) noexcept;

    static StatusOr<UploadSessionStore> Open(
        std::filesystem::path workspace_root,
        UploadQuota quota = {});

    StatusOr<UploadSession> CreateSession(
        const std::vector<UploadFileRequest>& files);

    StatusOr<UploadSession> GetSession(std::string_view session_id) const;

    StatusOr<UploadFile> GetFile(
        std::string_view session_id,
        std::string_view file_id) const;

    StatusOr<BeginUploadResult> BeginUpload(
        std::string_view session_id,
        std::string_view file_id);

    Status MarkUploadFailed(
        std::string_view session_id,
        std::string_view file_id);

    Status CommitUpload(
        std::string_view session_id,
        std::string_view file_id,
        std::uint64_t received_size,
        const Digest& server_digest);

    Status CompleteSession(std::string_view session_id);

    Status RecoverInterruptedUploads();

    std::filesystem::path TempPath(
        std::string_view session_id,
        std::string_view file_id) const;

    std::filesystem::path FinalPath(
        std::string_view session_id,
        std::string_view safe_filename) const;

private:
    UploadSessionStore(
        void* database,
        std::filesystem::path workspace_root,
        UploadQuota quota) noexcept;

    void* database_ = nullptr;
    std::filesystem::path workspace_root_;
    UploadQuota quota_;
};

}  // namespace photobridge
