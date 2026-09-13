#include "photobridge/model/association_graph.h"

#include <algorithm>
#include <map>
#include <numeric>
#include <utility>

namespace photobridge {
namespace {

bool ContainsVertex(
    const std::vector<PhysicalAssetId>& vertices,
    const PhysicalAssetId& id)
{
    return std::find(vertices.begin(), vertices.end(), id) != vertices.end();
}

bool SameEdge(
    const AssociationEdge& left,
    const AssociationEdge& right)
{
    return left.edge_kind == right.edge_kind
        && left.lhs == right.lhs
        && left.rhs == right.rhs
        && left.relation == right.relation
        && left.rule == right.rule
        && left.status == right.status
        && left.evidence.rule == right.evidence.rule
        && left.evidence.detail == right.evidence.detail;
}

}  // namespace

Status AssociationGraph::AddVertex(PhysicalAssetId id)
{
    if (id.empty()) {
        return Status(
            StatusCode::kInvalidArgument,
            "association graph vertex id must not be empty");
    }
    if (ContainsVertex(vertices_, id)) {
        return Status(
            StatusCode::kAlreadyExists,
            "association graph vertex already exists");
    }
    vertices_.push_back(std::move(id));
    return Status::Ok();
}

Status AssociationGraph::AddEdge(AssociationEdge edge)
{
    if (edge.lhs.empty() || edge.rhs.empty()) {
        return Status(
            StatusCode::kInvalidArgument,
            "association graph edge endpoints must not be empty");
    }
    if (!ContainsVertex(vertices_, edge.lhs)
        || !ContainsVertex(vertices_, edge.rhs)) {
        return Status(
            StatusCode::kNotFound,
            "association graph edge references an unknown vertex");
    }
    if (edge.lhs == edge.rhs) {
        return Status(
            StatusCode::kInvalidArgument,
            "association graph must not contain a self edge");
    }
    if (std::find_if(
            edges_.begin(),
            edges_.end(),
            [&edge](const AssociationEdge& existing) {
                return SameEdge(existing, edge);
            }) != edges_.end()) {
        return Status(
            StatusCode::kAlreadyExists,
            "association graph edge already exists");
    }
    edges_.push_back(std::move(edge));
    return Status::Ok();
}

StatusOr<std::vector<LogicalAsset>> AssociationGraph::BuildLogicalAssets() const
{
    std::vector<PhysicalAssetId> vertices = vertices_;
    std::sort(vertices.begin(), vertices.end());
    std::vector<std::size_t> parent(vertices.size());
    std::iota(parent.begin(), parent.end(), 0U);

    const auto find_root = [&parent](std::size_t index) {
        std::size_t root = index;
        while (parent[root] != root) root = parent[root];
        while (parent[index] != index) {
            const std::size_t next = parent[index];
            parent[index] = root;
            index = next;
        }
        return root;
    };

    std::vector<const AssociationEdge*> composition_edges;
    for (const AssociationEdge& edge : edges_) {
        if (edge.edge_kind == EdgeKind::kComposition
            && edge.status == AssociationStatus::kConfirmed) {
            composition_edges.push_back(&edge);
        }
    }
    std::sort(
        composition_edges.begin(),
        composition_edges.end(),
        [](const AssociationEdge* left, const AssociationEdge* right) {
            if (left->lhs != right->lhs) return left->lhs < right->lhs;
            if (left->rhs != right->rhs) return left->rhs < right->rhs;
            if (left->rule != right->rule) return left->rule < right->rule;
            return left->evidence.detail < right->evidence.detail;
        });

    for (const AssociationEdge* edge : composition_edges) {
        const auto left = std::lower_bound(vertices.begin(), vertices.end(), edge->lhs);
        const auto right = std::lower_bound(vertices.begin(), vertices.end(), edge->rhs);
        if (left == vertices.end() || right == vertices.end()
            || *left != edge->lhs || *right != edge->rhs) {
            return Status(
                StatusCode::kInternal,
                "association graph composition edge lost a vertex");
        }
        const std::size_t left_root = find_root(
            static_cast<std::size_t>(left - vertices.begin()));
        const std::size_t right_root = find_root(
            static_cast<std::size_t>(right - vertices.begin()));
        if (left_root == right_root) continue;
        if (left_root < right_root) parent[right_root] = left_root;
        else parent[left_root] = right_root;
    }

    std::map<std::size_t, std::vector<PhysicalAssetId>> groups;
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        groups[find_root(index)].push_back(vertices[index]);
    }

    std::vector<LogicalAsset> result;
    result.reserve(groups.size());
    for (auto& [root, members] : groups) {
        (void)root;
        auto logical_id = LogicalAssetIdForMembers(members);
        if (!logical_id.ok()) return logical_id.status();
        result.push_back(LogicalAsset{
            std::move(logical_id.value()),
            std::move(members),
            std::nullopt,
        });
    }
    std::sort(
        result.begin(),
        result.end(),
        [](const LogicalAsset& left, const LogicalAsset& right) {
            return left.id < right.id;
        });
    return result;
}

std::size_t AssociationGraph::vertex_count() const noexcept
{
    return vertices_.size();
}

std::size_t AssociationGraph::edge_count() const noexcept
{
    return edges_.size();
}

}  // namespace photobridge
