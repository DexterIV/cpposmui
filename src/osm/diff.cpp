#include "diff.hpp"
#include <algorithm>

namespace osm::diff {

std::vector<TagDiff> compute_tag_diffs(const TagMap& before, const TagMap& after) {
    std::vector<TagDiff> diffs;

    for (const auto& [k, v] : before) {
        if (auto it = after.find(k); it != after.end()) {
            if (it->second != v)
                diffs.push_back({k, v, it->second}); // modified
        } else {
            diffs.push_back({k, v, std::nullopt}); // removed
        }
    }
    for (const auto& [k, v] : after) {
        if (!before.contains(k))
            diffs.push_back({k, std::nullopt, v}); // added
    }
    std::ranges::sort(diffs, {}, &TagDiff::key);
    return diffs;
}

void enrich_changeset(ChangeSet& cs, const Dataset& base) {
    for (auto& d : cs.nodes) {
        if (d.after && base.nodes.contains(d.after->id)) {
            d.before = base.nodes.at(d.after->id);
            if (d.state == DiffState::Modified)
                d.tag_diffs = compute_tag_diffs(d.before->tags, d.after->tags);
        }
    }
    for (auto& d : cs.ways) {
        if (d.after && base.ways.contains(d.after->id)) {
            d.before = base.ways.at(d.after->id);
            if (d.state == DiffState::Modified)
                d.tag_diffs = compute_tag_diffs(d.before->tags, d.after->tags);
        }
    }
    for (auto& d : cs.relations) {
        if (d.after && base.relations.contains(d.after->id)) {
            d.before = base.relations.at(d.after->id);
            if (d.state == DiffState::Modified)
                d.tag_diffs = compute_tag_diffs(d.before->tags, d.after->tags);
        }
    }
}

Dataset apply_changeset(const Dataset& base, const ChangeSet& cs) {
    Dataset result = base;

    for (const auto& d : cs.nodes) {
        if (d.state == DiffState::Deleted && d.after)
            result.nodes.erase(d.after->id);
        else if (d.after)
            result.nodes[d.after->id] = *d.after;
    }
    for (const auto& d : cs.ways) {
        if (d.state == DiffState::Deleted && d.after)
            result.ways.erase(d.after->id);
        else if (d.after)
            result.ways[d.after->id] = *d.after;
    }
    for (const auto& d : cs.relations) {
        if (d.state == DiffState::Deleted && d.after)
            result.relations.erase(d.after->id);
        else if (d.after)
            result.relations[d.after->id] = *d.after;
    }
    return result;
}

ChangeSet diff_datasets(const Dataset& base, const Dataset& incoming) {
    ChangeSet cs;

    // Nodes
    for (const auto& [id, n] : incoming.nodes) {
        if (auto it = base.nodes.find(id); it != base.nodes.end()) {
            if (it->second.tags != n.tags || it->second.lat != n.lat || it->second.lon != n.lon) {
                ObjectDiff<Node> d;
                d.state = DiffState::Modified;
                d.before = it->second;
                d.after  = n;
                d.tag_diffs = compute_tag_diffs(it->second.tags, n.tags);
                cs.nodes.push_back(std::move(d));
            }
        } else {
            ObjectDiff<Node> d;
            d.state = DiffState::Added;
            d.after = n;
            cs.nodes.push_back(std::move(d));
        }
    }
    for (const auto& [id, n] : base.nodes) {
        if (!incoming.nodes.contains(id)) {
            ObjectDiff<Node> d;
            d.state  = DiffState::Deleted;
            d.before = n;
            cs.nodes.push_back(std::move(d));
        }
    }

    // Ways
    for (const auto& [id, w] : incoming.ways) {
        if (auto it = base.ways.find(id); it != base.ways.end()) {
            if (it->second.tags != w.tags || it->second.nodes != w.nodes) {
                ObjectDiff<Way> d;
                d.state = DiffState::Modified;
                d.before = it->second;
                d.after  = w;
                d.tag_diffs = compute_tag_diffs(it->second.tags, w.tags);
                cs.ways.push_back(std::move(d));
            }
        } else {
            ObjectDiff<Way> d;
            d.state = DiffState::Added;
            d.after = w;
            cs.ways.push_back(std::move(d));
        }
    }
    for (const auto& [id, w] : base.ways) {
        if (!incoming.ways.contains(id)) {
            ObjectDiff<Way> d;
            d.state  = DiffState::Deleted;
            d.before = w;
            cs.ways.push_back(std::move(d));
        }
    }

    return cs;
}

} // namespace osm::diff
