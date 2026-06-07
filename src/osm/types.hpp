#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <optional>

namespace osm {

using TagMap = std::unordered_map<std::string, std::string>;
using NodeId  = int64_t;
using WayId   = int64_t;
using RelId   = int64_t;

enum class ObjType { Node, Way, Relation };
enum class DiffState { Unchanged, Added, Modified, Deleted };

struct Node {
    NodeId id{};
    double lat{}, lon{};
    TagMap tags;
    int64_t version{};
    bool visible{true};
};

struct WayNode {
    NodeId ref{};
    bool operator==(const WayNode&) const = default;
};

struct Way {
    WayId id{};
    std::vector<WayNode> nodes;
    TagMap tags;
    int64_t version{};
    bool visible{true};
};

struct RelMember {
    ObjType type{};
    int64_t ref{};
    std::string role;
};

struct Relation {
    RelId id{};
    std::vector<RelMember> members;
    TagMap tags;
    int64_t version{};
    bool visible{true};
};

// ── Diff types ────────────────────────────────────────────────────────────────

struct TagDiff {
    std::string key;
    std::optional<std::string> old_val; // nullopt = key didn't exist
    std::optional<std::string> new_val; // nullopt = key removed
};

template<typename T>
struct ObjectDiff {
    DiffState state{DiffState::Unchanged};
    std::optional<T> before; // nullopt for Added
    std::optional<T> after;  // nullopt for Deleted
    std::vector<TagDiff> tag_diffs;
};

struct ChangeSet {
    std::vector<ObjectDiff<Node>>     nodes;
    std::vector<ObjectDiff<Way>>      ways;
    std::vector<ObjectDiff<Relation>> relations;
};

// ── Dataset ───────────────────────────────────────────────────────────────────

struct Dataset {
    std::unordered_map<NodeId, Node>   nodes;
    std::unordered_map<WayId,  Way>    ways;
    std::unordered_map<RelId,  Relation> relations;

    double min_lat{}, max_lat{}, min_lon{}, max_lon{};
};

} // namespace osm
