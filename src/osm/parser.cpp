#include "parser.hpp"
#include <pugixml.hpp>
#include <sstream>
#include <stdexcept>

namespace osm {

namespace {

TagMap parse_tags(const pugi::xml_node& parent) {
    TagMap tags;
    for (auto tag : parent.children("tag"))
        tags[tag.attribute("k").as_string()] = tag.attribute("v").as_string();
    return tags;
}

Node parse_node(const pugi::xml_node& n) {
    Node node;
    node.id      = n.attribute("id").as_llong();
    node.lat     = n.attribute("lat").as_double();
    node.lon     = n.attribute("lon").as_double();
    node.version = n.attribute("version").as_llong(1);
    node.visible = n.attribute("visible").as_bool(true);
    node.tags    = parse_tags(n);
    return node;
}

Way parse_way(const pugi::xml_node& w) {
    Way way;
    way.id      = w.attribute("id").as_llong();
    way.version = w.attribute("version").as_llong(1);
    way.visible = w.attribute("visible").as_bool(true);
    way.tags    = parse_tags(w);
    for (auto nd : w.children("nd"))
        way.nodes.push_back({nd.attribute("ref").as_llong()});
    return way;
}

Relation parse_relation(const pugi::xml_node& r) {
    Relation rel;
    rel.id      = r.attribute("id").as_llong();
    rel.version = r.attribute("version").as_llong(1);
    rel.visible = r.attribute("visible").as_bool(true);
    rel.tags    = parse_tags(r);
    for (auto m : r.children("member")) {
        RelMember mem;
        std::string type_str = m.attribute("type").as_string();
        if      (type_str == "node")     mem.type = ObjType::Node;
        else if (type_str == "way")      mem.type = ObjType::Way;
        else                             mem.type = ObjType::Relation;
        mem.ref  = m.attribute("ref").as_llong();
        mem.role = m.attribute("role").as_string();
        rel.members.push_back(std::move(mem));
    }
    return rel;
}

void write_tags(pugi::xml_node& el, const TagMap& tags) {
    for (const auto& [k, v] : tags) {
        auto t = el.append_child("tag");
        t.append_attribute("k") = k.c_str();
        t.append_attribute("v") = v.c_str();
    }
}

void write_node(pugi::xml_node& parent, const Node& n, long long cs_id) {
    auto el = parent.append_child("node");
    el.append_attribute("id")      = (long long)n.id;
    el.append_attribute("lat")     = n.lat;
    el.append_attribute("lon")     = n.lon;
    el.append_attribute("version") = (long long)n.version;
    if (cs_id) el.append_attribute("changeset") = cs_id;
    write_tags(el, n.tags);
}

void write_way(pugi::xml_node& parent, const Way& w, long long cs_id) {
    auto el = parent.append_child("way");
    el.append_attribute("id")      = (long long)w.id;
    el.append_attribute("version") = (long long)w.version;
    if (cs_id) el.append_attribute("changeset") = cs_id;
    for (const auto& nd : w.nodes)
        el.append_child("nd").append_attribute("ref") = (long long)nd.ref;
    write_tags(el, w.tags);
}

// Build the <osmChange> document. cs_id != 0 is stamped on every element.
void build_osmchange(pugi::xml_document& doc, const ChangeSet& cs, long long cs_id) {
    auto root = doc.append_child("osmChange");
    root.append_attribute("version")   = "0.6";
    root.append_attribute("generator") = "cpposmui";

    pugi::xml_node create;
    auto create_blk = [&]() -> pugi::xml_node& {
        if (!create) create = root.append_child("create");
        return create;
    };
    for (const auto& d : cs.nodes)
        if (d.state == DiffState::Added && d.after) write_node(create_blk(), *d.after, cs_id);
    for (const auto& d : cs.ways)
        if (d.state == DiffState::Added && d.after) write_way(create_blk(), *d.after, cs_id);

    pugi::xml_node modify;
    auto modify_blk = [&]() -> pugi::xml_node& {
        if (!modify) modify = root.append_child("modify");
        return modify;
    };
    for (const auto& d : cs.nodes)
        if (d.state == DiffState::Modified && d.after) write_node(modify_blk(), *d.after, cs_id);
    for (const auto& d : cs.ways)
        if (d.state == DiffState::Modified && d.after) write_way(modify_blk(), *d.after, cs_id);

    pugi::xml_node del;
    auto delete_blk = [&]() -> pugi::xml_node& {
        if (!del) del = root.append_child("delete");
        return del;
    };
    for (const auto& d : cs.ways)
        if (d.state == DiffState::Deleted && d.before) write_way(delete_blk(), *d.before, cs_id);
    for (const auto& d : cs.nodes)
        if (d.state == DiffState::Deleted && d.before) write_node(delete_blk(), *d.before, cs_id);
}

} // anonymous namespace

std::expected<Dataset, ParseError>
load_osm(const std::filesystem::path& path) {
    pugi::xml_document doc;
    auto result = doc.load_file(path.string().c_str());
    if (!result)
        return std::unexpected(ParseError::XmlError);

    auto root = doc.child("osm");
    if (!root)
        return std::unexpected(ParseError::UnknownFormat);

    Dataset ds;
    ds.min_lat =  90.0; ds.max_lat = -90.0;
    ds.min_lon = 180.0; ds.max_lon = -180.0;

    // bounds element (optional)
    if (auto b = root.child("bounds"); b) {
        ds.min_lat = b.attribute("minlat").as_double();
        ds.max_lat = b.attribute("maxlat").as_double();
        ds.min_lon = b.attribute("minlon").as_double();
        ds.max_lon = b.attribute("maxlon").as_double();
    }

    for (auto child : root.children()) {
        std::string_view name = child.name();
        if (name == "node") {
            auto n = parse_node(child);
            if (ds.min_lat > 90.0) { // first node, init bounds
                ds.min_lat = ds.max_lat = n.lat;
                ds.min_lon = ds.max_lon = n.lon;
            } else {
                ds.min_lat = std::min(ds.min_lat, n.lat);
                ds.max_lat = std::max(ds.max_lat, n.lat);
                ds.min_lon = std::min(ds.min_lon, n.lon);
                ds.max_lon = std::max(ds.max_lon, n.lon);
            }
            ds.nodes[n.id] = std::move(n);
        } else if (name == "way") {
            auto w = parse_way(child);
            ds.ways[w.id] = std::move(w);
        } else if (name == "relation") {
            auto r = parse_relation(child);
            ds.relations[r.id] = std::move(r);
        }
    }
    return ds;
}

std::expected<ChangeSet, ParseError>
load_osc(const std::filesystem::path& path) {
    pugi::xml_document doc;
    auto result = doc.load_file(path.string().c_str());
    if (!result)
        return std::unexpected(ParseError::XmlError);

    auto root = doc.child("osmChange");
    if (!root)
        return std::unexpected(ParseError::UnknownFormat);

    ChangeSet cs;

    auto process_block = [&](const pugi::xml_node& block, DiffState state) {
        for (auto child : block.children()) {
            std::string_view name = child.name();
            if (name == "node") {
                ObjectDiff<Node> d;
                d.state = state;
                d.after = parse_node(child);
                cs.nodes.push_back(std::move(d));
            } else if (name == "way") {
                ObjectDiff<Way> d;
                d.state = state;
                d.after = parse_way(child);
                cs.ways.push_back(std::move(d));
            } else if (name == "relation") {
                ObjectDiff<Relation> d;
                d.state = state;
                d.after = parse_relation(child);
                cs.relations.push_back(std::move(d));
            }
        }
    };

    for (auto block : root.children()) {
        std::string_view bname = block.name();
        if      (bname == "create") process_block(block, DiffState::Added);
        else if (bname == "modify") process_block(block, DiffState::Modified);
        else if (bname == "delete") process_block(block, DiffState::Deleted);
    }
    return cs;
}

bool write_osc(const ChangeSet& cs, const std::filesystem::path& path) {
    pugi::xml_document doc;
    build_osmchange(doc, cs, 0);
    return doc.save_file(path.string().c_str());
}

std::string changeset_xml(const ChangeSet& cs, long long changeset_id) {
    pugi::xml_document doc;
    build_osmchange(doc, cs, changeset_id);
    std::ostringstream ss;
    doc.save(ss, "", pugi::format_raw);
    return ss.str();
}

} // namespace osm
