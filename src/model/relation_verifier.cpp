#include "photobridge/model/relation_verifier.h"

namespace photobridge {
namespace {

bool Equal(const PhotoRelationship& left, const PhotoRelationship& right)
{
    return left.lhs == right.lhs
        && left.rhs == right.rhs
        && left.relation == right.relation
        && left.status == right.status
        && left.rule == right.rule
        && left.evidence.rule == right.evidence.rule
        && left.evidence.detail == right.evidence.detail;
}

}  // namespace

RelationVerificationResult VerifyRelations(
    const std::optional<std::span<const PhotoRelationship>>& expected,
    const std::optional<std::span<const PhotoRelationship>>& actual)
{
    if (!expected.has_value() || !actual.has_value()) {
        return {
            RelationVerification::kUnverifiable,
            "expected or actual relationships are unavailable",
        };
    }
    if (expected->size() != actual->size()) {
        return {
            RelationVerification::kMismatch,
            "relationship counts differ",
        };
    }
    for (std::size_t index = 0; index < expected->size(); ++index) {
        if (!Equal((*expected)[index], (*actual)[index])) {
            return {
                RelationVerification::kMismatch,
                "relationship values differ",
            };
        }
    }
    return {
        RelationVerification::kIdentical,
        "relationships match",
    };
}

}  // namespace photobridge
