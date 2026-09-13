#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

#include "photobridge/common/status.h"
#include "photobridge/model/physical_asset.h"

namespace photobridge {

enum class MetadataField {
    kTitle,
    kDescription,
    kFavorite,
    kTakenTime,
};

enum class MetadataSource {
    kGoogleTakeoutJson,
};

enum class TimePrecision {
    kSecond,
};

struct AbsoluteTime {
    std::int64_t unix_ns = 0;
};

struct LocalDateTime {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    std::int32_t nanosecond = 0;
};

struct TimeCandidate {
    std::variant<AbsoluteTime, LocalDateTime> value;
    std::optional<std::int32_t> utc_offset_minutes;
    TimePrecision precision = TimePrecision::kSecond;
    MetadataSource source = MetadataSource::kGoogleTakeoutJson;
    std::string raw_value;
};

Status ValidateTimeCandidate(const TimeCandidate& candidate) noexcept;

struct GeoPoint {
    double latitude = 0.0;
    double longitude = 0.0;
};

using MetadataValue = std::variant<
    std::string,
    std::int64_t,
    double,
    bool,
    TimeCandidate,
    GeoPoint>;
using RuleId = std::string;

enum class MetadataValueKind {
    kString,
    kInt64,
    kDouble,
    kBool,
    kTimeCandidate,
    kGeoPoint,
};

MetadataValueKind MetadataValueKindOf(const MetadataValue& value) noexcept;

bool IsMetadataValueCompatible(
    MetadataField field,
    const MetadataValue& value) noexcept;

bool MetadataValuesEqual(
    const MetadataValue& left,
    const MetadataValue& right) noexcept;

struct MetadataCandidate {
    PhysicalAssetId asset_id;
    MetadataField field = MetadataField::kTitle;
    MetadataValue value;
    MetadataSource source = MetadataSource::kGoogleTakeoutJson;
    RuleId extraction_rule;
    std::string evidence;
};

}  // namespace photobridge
