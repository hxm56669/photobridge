#include "photobridge/model/metadata_candidate.h"

#include <type_traits>
#include <variant>

namespace photobridge {

Status ValidateTimeCandidate(const TimeCandidate& candidate) noexcept
{
    if (candidate.raw_value.empty()) {
        return Status(
            StatusCode::kInvalidArgument,
            "time candidate raw value must not be empty");
    }
    if (candidate.precision != TimePrecision::kSecond) {
        return Status(
            StatusCode::kInvalidArgument,
            "unsupported time candidate precision");
    }
    if (candidate.source != MetadataSource::kGoogleTakeoutJson) {
        return Status(
            StatusCode::kInvalidArgument,
            "unsupported time candidate source");
    }
    if (candidate.utc_offset_minutes.has_value()
        && (candidate.utc_offset_minutes.value() < -14 * 60
            || candidate.utc_offset_minutes.value() > 14 * 60)) {
        return Status(
            StatusCode::kInvalidArgument,
            "time candidate UTC offset is outside the supported range");
    }

    if (const auto* local = std::get_if<LocalDateTime>(&candidate.value)) {
        if (local->year < 1 || local->year > 9999
            || local->month < 1 || local->month > 12
            || local->day < 1 || local->day > 31
            || local->hour < 0 || local->hour > 23
            || local->minute < 0 || local->minute > 59
            || local->second < 0 || local->second > 59
            || local->nanosecond < 0
            || local->nanosecond > 999999999) {
            return Status(
                StatusCode::kInvalidArgument,
                "local time candidate has an invalid calendar component");
        }
    }
    return Status::Ok();
}

MetadataValueKind MetadataValueKindOf(const MetadataValue& value) noexcept
{
    return std::visit(
        [](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, std::string>) {
                return MetadataValueKind::kString;
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                return MetadataValueKind::kInt64;
            } else if constexpr (std::is_same_v<T, double>) {
                return MetadataValueKind::kDouble;
            } else if constexpr (std::is_same_v<T, bool>) {
                return MetadataValueKind::kBool;
            } else if constexpr (std::is_same_v<T, TimeCandidate>) {
                return MetadataValueKind::kTimeCandidate;
            } else {
                return MetadataValueKind::kGeoPoint;
            }
        },
        value);
}

bool IsMetadataValueCompatible(
    MetadataField field,
    const MetadataValue& value) noexcept
{
    const MetadataValueKind kind = MetadataValueKindOf(value);
    switch (field) {
    case MetadataField::kTitle:
    case MetadataField::kDescription:
        return kind == MetadataValueKind::kString;
    case MetadataField::kFavorite:
        return kind == MetadataValueKind::kBool;
    case MetadataField::kTakenTime:
        return kind == MetadataValueKind::kTimeCandidate;
    }
    return false;
}

bool MetadataValuesEqual(
    const MetadataValue& left,
    const MetadataValue& right) noexcept
{
    if (MetadataValueKindOf(left) != MetadataValueKindOf(right)) return false;
    return std::visit(
        [](const auto& left_value, const auto& right_value) {
            using Left = std::decay_t<decltype(left_value)>;
            using Right = std::decay_t<decltype(right_value)>;
            if constexpr (!std::is_same_v<Left, Right>) {
                return false;
            } else if constexpr (std::is_same_v<Left, TimeCandidate>) {
                const bool same_time = std::visit(
                    [](const auto& left_time, const auto& right_time) {
                        using LeftTime = std::decay_t<decltype(left_time)>;
                        using RightTime = std::decay_t<decltype(right_time)>;
                        if constexpr (!std::is_same_v<LeftTime, RightTime>) {
                            return false;
                        } else if constexpr (
                            std::is_same_v<LeftTime, AbsoluteTime>) {
                            return left_time.unix_ns == right_time.unix_ns;
                        } else {
                            return left_time.year == right_time.year
                                && left_time.month == right_time.month
                                && left_time.day == right_time.day
                                && left_time.hour == right_time.hour
                                && left_time.minute == right_time.minute
                                && left_time.second == right_time.second
                                && left_time.nanosecond
                                    == right_time.nanosecond;
                        }
                    },
                    left_value.value,
                    right_value.value);
                return same_time
                    && left_value.utc_offset_minutes
                        == right_value.utc_offset_minutes
                    && left_value.precision == right_value.precision
                    && left_value.source == right_value.source
                    && left_value.raw_value == right_value.raw_value;
            } else if constexpr (std::is_same_v<Left, GeoPoint>) {
                return left_value.latitude == right_value.latitude
                    && left_value.longitude == right_value.longitude;
            } else {
                return left_value == right_value;
            }
        },
        left,
        right);
}

}  // namespace photobridge
