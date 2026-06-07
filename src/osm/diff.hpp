#pragma once
#include "types.hpp"

namespace osm::diff {

// Compute tag-level diffs between two tag maps
std::vector<TagDiff> compute_tag_diffs(const TagMap& before, const TagMap& after);

// Given a base dataset and a loaded .osc changeset, fill in the 'before'
// states of each ObjectDiff from the existing dataset.
void enrich_changeset(ChangeSet& cs, const Dataset& base);

// Apply a changeset on top of a dataset (returns new dataset)
Dataset apply_changeset(const Dataset& base, const ChangeSet& cs);

// Diff two full datasets (e.g. base vs. imported version)
ChangeSet diff_datasets(const Dataset& base, const Dataset& incoming);

} // namespace osm::diff
