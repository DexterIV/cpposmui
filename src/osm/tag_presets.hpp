#pragma once
// Curated list of common OSM tag keys and their common values, used to power
// autocomplete / "tag search" in the tag editor. Not exhaustive — just the
// everyday tags an editor reaches for most.
#include <array>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace osm::presets {

inline constexpr std::array<std::string_view, 48> common_keys = {
    "name", "amenity", "highway", "building", "shop", "natural", "landuse",
    "leisure", "surface", "barrier", "waterway", "power", "railway", "man_made",
    "tourism", "office", "craft", "historic", "addr:street", "addr:housenumber",
    "addr:city", "addr:postcode", "addr:country", "maxspeed", "oneway", "lanes",
    "access", "service", "bridge", "tunnel", "layer", "ref", "operator",
    "website", "phone", "opening_hours", "wheelchair", "height", "width",
    "material", "religion", "sport", "cuisine", "brand", "network",
    "public_transport", "bicycle", "foot",
};

// Common values per key. Keys absent here simply have no value suggestions.
inline const std::unordered_map<std::string_view, std::vector<std::string_view>> key_values = {
    {"highway", {"residential", "service", "footway", "path", "primary", "secondary",
                 "tertiary", "unclassified", "track", "cycleway", "living_street",
                 "pedestrian", "steps", "motorway", "trunk", "crossing",
                 "traffic_signals", "bus_stop", "turning_circle"}},
    {"building", {"yes", "house", "residential", "apartments", "detached", "garage",
                  "commercial", "industrial", "retail", "school", "church",
                  "hut", "roof", "shed", "construction", "warehouse"}},
    {"amenity", {"parking", "restaurant", "cafe", "fast_food", "bench", "school",
                 "place_of_worship", "fuel", "pharmacy", "bank", "atm", "toilets",
                 "drinking_water", "waste_basket", "bicycle_parking", "kindergarten",
                 "hospital", "bar", "pub", "post_office", "library", "townhall"}},
    {"surface", {"asphalt", "paved", "unpaved", "concrete", "paving_stones",
                 "gravel", "ground", "grass", "dirt", "sand", "cobblestone",
                 "wood", "compacted", "fine_gravel"}},
    {"natural", {"tree", "water", "wood", "scrub", "grassland", "wetland", "peak",
                 "rock", "cliff", "beach", "bare_rock", "spring", "tree_row"}},
    {"landuse", {"residential", "commercial", "industrial", "retail", "grass",
                 "forest", "meadow", "farmland", "farmyard", "cemetery",
                 "construction", "recreation_ground", "allotments", "orchard"}},
    {"leisure", {"park", "garden", "playground", "pitch", "sports_centre",
                 "swimming_pool", "fitness_centre", "stadium", "track", "dog_park"}},
    {"barrier", {"fence", "wall", "gate", "hedge", "bollard", "kerb",
                 "retaining_wall", "lift_gate", "block", "cycle_barrier"}},
    {"oneway", {"yes", "no", "-1"}},
    {"access", {"yes", "no", "private", "permissive", "destination", "customers",
                "agricultural", "delivery"}},
    {"shop", {"convenience", "supermarket", "bakery", "hairdresser", "clothes",
              "car_repair", "kiosk", "butcher", "florist", "doityourself",
              "electronics", "mobile_phone", "greengrocer", "beauty"}},
    {"waterway", {"stream", "river", "ditch", "drain", "canal", "riverbank",
                  "weir", "dam"}},
    {"service", {"driveway", "parking_aisle", "alley", "sidewalk", "spur", "yard"}},
    {"railway", {"rail", "tram", "subway", "light_rail", "station", "halt",
                 "tram_stop", "switch", "level_crossing", "buffer_stop"}},
    {"man_made", {"tower", "mast", "pier", "water_tower", "chimney", "storage_tank",
                  "surveillance", "street_cabinet"}},
    {"power", {"tower", "pole", "line", "minor_line", "substation", "transformer",
               "generator"}},
    {"tourism", {"hotel", "guest_house", "attraction", "viewpoint", "museum",
                 "information", "artwork", "picnic_site", "camp_site"}},
    {"wheelchair", {"yes", "no", "limited"}},
    {"bicycle", {"yes", "no", "designated", "dismount", "permissive"}},
    {"foot", {"yes", "no", "designated", "permissive"}},
    {"bridge", {"yes", "viaduct", "aqueduct", "boardwalk"}},
    {"tunnel", {"yes", "culvert", "building_passage"}},
    {"sport", {"soccer", "tennis", "basketball", "swimming", "running",
               "multi", "fitness", "table_tennis"}},
    {"religion", {"christian", "muslim", "jewish", "buddhist", "hindu"}},
};

} // namespace osm::presets
