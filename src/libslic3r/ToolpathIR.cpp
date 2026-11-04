
// ToolpathIR.cpp — Toolpath IR export/import implementation
//
// Spec reference: docs/ORCASLICER_IR_PLUGIN.md §4, §5, §6, §10, §11
//
// Uses nlohmann/json (already in OrcaSlicer's dependency tree — see §14).
// JSON format is used for initial development; migration to MessagePack is
// Phase 7 (§15).

#include "ToolpathIR.hpp"

#include "PrintObject.hpp"
#include "Print.hpp"
#include "Layer.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "libslic3r.h"       // scale_(), unscale()

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>
#include <cassert>

using json = nlohmann::json;

namespace Slic3r {

// =========================================================================
// ExtrusionRole string mapping (§2.3)
// =========================================================================

static const std::pair<ExtrusionRole, const char*> s_role_names[] = {
    { erNone,                    "erNone"                    },
    { erPerimeter,               "erPerimeter"               },
    { erExternalPerimeter,       "erExternalPerimeter"       },
    { erOverhangPerimeter,       "erOverhangPerimeter"       },
    { erInternalInfill,          "erInternalInfill"          },
    { erSolidInfill,             "erSolidInfill"             },
    { erTopSolidInfill,          "erTopSolidInfill"          },
    { erBottomSurface,           "erBottomSurface"           },
    { erIroning,                 "erIroning"                 },
    { erBridgeInfill,            "erBridgeInfill"            },
    { erInternalBridgeInfill,    "erInternalBridgeInfill"    },
    { erGapFill,                 "erGapFill"                 },
    { erSkirt,                   "erSkirt"                   },
    { erBrim,                    "erBrim"                    },
    { erSupportMaterial,         "erSupportMaterial"         },
    { erSupportMaterialInterface,"erSupportMaterialInterface"},
    { erSupportTransition,       "erSupportTransition"       },
    { erWipeTower,               "erWipeTower"               },
    { erCustom,                  "erCustom"                  },
    { erMixed,                   "erMixed"                   },
};

const char* extrusion_role_to_string(ExtrusionRole role)
{
    for (const auto &[r, name] : s_role_names)
        if (r == role) return name;
    return "erNone";  // fallback
}

ExtrusionRole string_to_extrusion_role(const std::string &s)
{
    for (const auto &[r, name] : s_role_names)
        if (s == name) return r;
    return erNone;  // fallback — unknown roles degrade to erNone
}

// =========================================================================
// ExtrusionLoopRole string mapping (§10.6)
// =========================================================================

static const std::pair<ExtrusionLoopRole, const char*> s_loop_role_names[] = {
    { elrDefault,  "elrDefault"  },
    { elrHole,     "elrHole"     },
    { elrInternal, "elrInternal" },
    { elrSkirt,    "elrSkirt"    },
};

const char* loop_role_to_string(ExtrusionLoopRole role)
{
    for (const auto &[r, name] : s_loop_role_names)
        if (r == role) return name;
    return "elrDefault";
}

ExtrusionLoopRole string_to_loop_role(const std::string &s)
{
    for (const auto &[r, name] : s_loop_role_names)
        if (s == name) return r;
    return elrDefault;
}

// =========================================================================
// Export helpers
// =========================================================================

// Serialise polyline points as unscaled mm (§2.2, §4.4, §5.4)
static json serialise_points(const Polyline3 &polyline)
{
    json pts = json::array();
    for (const Point3 &p : polyline.points) {
        pts.push_back({
            unscale(p.x()),
            unscale(p.y()),
            unscale(p.z())
        });
    }
    return pts;
}

// Serialise the common path fields shared by all ExtrusionPath variants (§4.4)
static json serialise_path_fields(const ExtrusionPath &path)
{
    json j;
    j["role"]            = extrusion_role_to_string(path.role());
    j["mm3_per_mm"]      = path.mm3_per_mm;
    j["width"]           = path.width;
    j["height"]          = path.height;
    j["overhang_degree"] = path.overhang_degree;
    j["curve_degree"]    = path.curve_degree;
    j["smooth_speed"]    = path.smooth_speed;
    j["z_contoured"]     = path.z_contoured;
    j["inset_idx"]       = path.inset_idx;
    j["can_reverse"]     = path.can_reverse();
    j["no_extrusion"]    = path.is_force_no_extrusion();
    j["points"]          = serialise_points(path.polyline);

    // Extension fields — null on export (§4.5)
    j["material_id"]          = nullptr;
    j["feed_state"]           = nullptr;
    j["is_transition"]        = false;
    j["transition_progress"]  = nullptr;
    j["nozzle_temp"]          = nullptr;

    return j;
}

// Serialise a single entity, dispatching by runtime type.
// CRITICAL: derived types tested before base types (§11).
static json serialise_entity(const ExtrusionEntity *entity);

static json serialise_collection(const ExtrusionEntityCollection &collection)
{
    json j;
    j["type"]    = "collection";
    j["no_sort"] = collection.no_sort;

    json entities = json::array();
    for (const ExtrusionEntity *entity : collection.entities)
        entities.push_back(serialise_entity(entity));
    j["entities"] = std::move(entities);

    return j;
}

static json serialise_entity(const ExtrusionEntity *entity)
{
    // --- Recursive collection ---
    if (auto *coll = dynamic_cast<const ExtrusionEntityCollection*>(entity))
        return serialise_collection(*coll);

    // --- ExtrusionLoopSloped (before ExtrusionLoop) ---
    if (auto *loop_sloped = dynamic_cast<const ExtrusionLoopSloped*>(entity)) {
        json j;
        j["type"]               = "loop_sloped";
        j["loop_role"]          = loop_role_to_string(loop_sloped->loop_role());
        j["print_after_infill"] = loop_sloped->print_after_infill;
        j["inset_idx"]          = loop_sloped->inset_idx;

        // Base loop paths
        json paths = json::array();
        for (const ExtrusionPath &p : loop_sloped->paths) {
            json pj    = serialise_path_fields(p);
            pj["type"] = "path";
            paths.push_back(std::move(pj));
        }
        j["paths"] = std::move(paths);

        // Sloped start/end vectors
        json starts = json::array();
        for (const ExtrusionPathSloped &sp : loop_sloped->starts) {
            json sj    = serialise_path_fields(sp);
            sj["type"] = "path_sloped";
            sj["slope_begin"] = { {"z_ratio", sp.slope_begin.z_ratio},
                                  {"e_ratio", sp.slope_begin.e_ratio} };
            sj["slope_end"]   = { {"z_ratio", sp.slope_end.z_ratio},
                                  {"e_ratio", sp.slope_end.e_ratio} };
            starts.push_back(std::move(sj));
        }
        j["starts"] = std::move(starts);

        json ends = json::array();
        for (const ExtrusionPathSloped &sp : loop_sloped->ends) {
            json ej    = serialise_path_fields(sp);
            ej["type"] = "path_sloped";
            ej["slope_begin"] = { {"z_ratio", sp.slope_begin.z_ratio},
                                  {"e_ratio", sp.slope_begin.e_ratio} };
            ej["slope_end"]   = { {"z_ratio", sp.slope_end.z_ratio},
                                  {"e_ratio", sp.slope_end.e_ratio} };
            ends.push_back(std::move(ej));
        }
        j["ends"] = std::move(ends);

        return j;
    }

    // --- ExtrusionLoop ---
    if (auto *loop = dynamic_cast<const ExtrusionLoop*>(entity)) {
        json j;
        j["type"]               = "loop";
        j["loop_role"]          = loop_role_to_string(loop->loop_role());
        j["print_after_infill"] = loop->print_after_infill;
        j["inset_idx"]          = loop->inset_idx;

        json paths = json::array();
        for (const ExtrusionPath &p : loop->paths) {
            json pj    = serialise_path_fields(p);
            pj["type"] = "path";
            paths.push_back(std::move(pj));
        }
        j["paths"] = std::move(paths);

        return j;
    }

    // --- ExtrusionMultiPath ---
    if (auto *mp = dynamic_cast<const ExtrusionMultiPath*>(entity)) {
        json j;
        j["type"]        = "multipath";
        j["can_reverse"] = mp->can_reverse();
        j["inset_idx"]   = mp->inset_idx;

        json paths = json::array();
        for (const ExtrusionPath &p : mp->paths) {
            json pj    = serialise_path_fields(p);
            pj["type"] = "path";
            paths.push_back(std::move(pj));
        }
        j["paths"] = std::move(paths);

        return j;
    }

    // --- ExtrusionPathSloped (before ExtrusionPath) ---
    if (auto *ps = dynamic_cast<const ExtrusionPathSloped*>(entity)) {
        json j         = serialise_path_fields(*ps);
        j["type"]      = "path_sloped";
        j["slope_begin"] = { {"z_ratio", ps->slope_begin.z_ratio},
                             {"e_ratio", ps->slope_begin.e_ratio} };
        j["slope_end"]   = { {"z_ratio", ps->slope_end.z_ratio},
                             {"e_ratio", ps->slope_end.e_ratio} };
        return j;
    }

    // --- ExtrusionPathContoured (before ExtrusionPath) ---
    if (auto *pc = dynamic_cast<const ExtrusionPathContoured*>(entity)) {
        json j    = serialise_path_fields(*pc);
        j["type"] = "path_contoured";

        json z_diffs = json::array();
        for (double zd : pc->z_diffs)
            z_diffs.push_back(zd);
        j["z_diffs"] = std::move(z_diffs);

        return j;
    }

    // --- ExtrusionPathOriented (before ExtrusionPath) ---
    if (auto *po = dynamic_cast<const ExtrusionPathOriented*>(entity)) {
        json j    = serialise_path_fields(*po);
        j["type"] = "path_oriented";
        return j;
    }

    // --- ExtrusionPath (base leaf) ---
    if (auto *p = dynamic_cast<const ExtrusionPath*>(entity)) {
        json j    = serialise_path_fields(*p);
        j["type"] = "path";
        return j;
    }

    throw Slic3r::RuntimeError("ToolpathIR export: unrecognised ExtrusionEntity type");
}

// =========================================================================
// export_toolpath_ir (§4.2, §4.3)
// =========================================================================

bool export_toolpath_ir(
    const std::vector<PrintObject*> &objects,
    const std::string               &output_path)
{
    json root;
    root["version"]      = 1;
    root["format"]       = "json";
    root["scale_factor"] = 1.0;

    json j_objects = json::array();

    for (size_t obj_idx = 0; obj_idx < objects.size(); ++obj_idx) {
        const PrintObject *obj = objects[obj_idx];
        json j_obj;
        j_obj["object_id"] = static_cast<int>(obj_idx);

        // ------ Object layers (§4.3) ------
        json j_layers = json::array();
        for (const Layer *layer : obj->layers()) {
            json j_layer;
            j_layer["layer_id"] = static_cast<int>(layer->id());
            j_layer["print_z"]  = layer->print_z;
            j_layer["height"]   = layer->height;
            j_layer["slice_z"]  = layer->slice_z;

            json j_regions = json::array();
            for (size_t r = 0; r < layer->region_count(); ++r) {
                const LayerRegion *region = layer->get_region(r);
                json j_region;
                j_region["region_idx"]  = static_cast<int>(r);
                j_region["extruder_id"] = region->region().extruder(FlowRole::frExternalPerimeter);
                j_region["perimeters"]  = serialise_collection(region->perimeters);
                j_region["fills"]       = serialise_collection(region->fills);
                j_region["thin_fills"]  = serialise_collection(region->thin_fills);
                j_regions.push_back(std::move(j_region));
            }
            j_layer["regions"] = std::move(j_regions);
            j_layers.push_back(std::move(j_layer));
        }
        j_obj["layers"] = std::move(j_layers);

        // ------ Support layers (§4.3) ------
        json j_support = json::array();
        for (const SupportLayer *sl : obj->support_layers()) {
            json j_sl;
            j_sl["layer_id"]      = static_cast<int>(sl->id());
            j_sl["print_z"]       = sl->print_z;
            j_sl["height"]        = sl->height;
            j_sl["support_fills"] = serialise_collection(sl->support_fills);
            j_support.push_back(std::move(j_sl));
        }
        j_obj["support_layers"] = std::move(j_support);

        j_objects.push_back(std::move(j_obj));
    }

    root["objects"] = std::move(j_objects);

    // Write to file
    std::ofstream ofs(output_path);
    if (!ofs.is_open())
        return false;
    ofs << root.dump();  // compact — no pretty-print for production speed
    return ofs.good();
}

// =========================================================================
// Import helpers
// =========================================================================

// Deserialise polyline points, rescaling from mm back to internal coords (§5.4)
static Polyline3 deserialise_points(const json &j_points)
{
    Polyline3 poly;
    poly.points.reserve(j_points.size());
    for (const auto &pt : j_points) {
        Point3 p;
        p.x() = scale_(pt[0].get<double>());
        p.y() = scale_(pt[1].get<double>());
        p.z() = scale_(pt[2].get<double>());
        poly.points.push_back(p);
    }
    return poly;
}

// Populate common path fields from JSON into an existing ExtrusionPath (§5.1)
static void deserialise_path_fields(const json &j, ExtrusionPath &path)
{
    path.set_role(string_to_extrusion_role(j.at("role").get<std::string>()));
    path.mm3_per_mm      = j.at("mm3_per_mm").get<double>();
    path.width           = j.at("width").get<float>();
    path.height          = j.at("height").get<float>();
    path.overhang_degree = j.at("overhang_degree").get<double>();
    path.curve_degree    = j.at("curve_degree").get<int>();
    path.smooth_speed    = j.at("smooth_speed").get<double>();
    path.z_contoured     = j.at("z_contoured").get<bool>();
    path.inset_idx       = j.at("inset_idx").get<int>();
    path.set_can_reverse(j.at("can_reverse").get<bool>());
    path.set_force_no_extrusion(j.at("no_extrusion").get<bool>());
    path.polyline        = deserialise_points(j.at("points"));
}

// Extract MaterialAnnotation from extension fields if any are non-null (§6.3).
// Returns true if an annotation was populated.
static bool extract_annotation(const json &j, MaterialAnnotation &ann)
{
    bool has_data = false;

    if (j.contains("material_id") && !j["material_id"].is_null()) {
        ann.material_id = j["material_id"].get<std::string>();
        has_data = true;
    }
    if (j.contains("feed_state") && !j["feed_state"].is_null()) {
        for (auto &[key, val] : j["feed_state"].items())
            ann.feed_state[key] = val.get<double>();
        has_data = true;
    }
    if (j.contains("is_transition") && j["is_transition"].get<bool>()) {
        ann.is_transition = true;
        has_data = true;
    }
    if (j.contains("transition_progress") && !j["transition_progress"].is_null()) {
        ann.transition_progress = j["transition_progress"].get<double>();
        has_data = true;
    }
    if (j.contains("nozzle_temp") && !j["nozzle_temp"].is_null()) {
        ann.nozzle_temp = j["nozzle_temp"].get<double>();
        has_data = true;
    }

    return has_data;
}

// Forward declaration for recursive deserialisation
static void deserialise_collection(const json &j,
                                   ExtrusionEntityCollection &collection,
                                   MaterialAnnotationMap &annotations);

// Deserialise a single entity from JSON, allocate on heap, push into parent.
// Returns the raw pointer for annotation map keying.
static ExtrusionEntity* deserialise_entity(const json &j,
                                           MaterialAnnotationMap &annotations)
{
    const std::string type = j.at("type").get<std::string>();

    // --- collection (recursive) ---
    if (type == "collection") {
        auto *coll = new ExtrusionEntityCollection();
        deserialise_collection(j, *coll, annotations);
        return coll;
    }

    // --- loop_sloped ---
    if (type == "loop_sloped") {
        auto *loop = new ExtrusionLoopSloped();
        loop->set_loop_role(string_to_loop_role(
            j.at("loop_role").get<std::string>()));
        loop->print_after_infill = j.at("print_after_infill").get<bool>();
        loop->inset_idx          = j.at("inset_idx").get<int>();

        // Base paths
        for (const auto &pj : j.at("paths")) {
            ExtrusionPath p;
            deserialise_path_fields(pj, p);
            loop->paths.push_back(std::move(p));

            // Check sub-path extension fields and annotate via the
            // parent loop pointer (the loop is the entity in the collection)
        }

        // Sloped starts
        for (const auto &sj : j.at("starts")) {
            ExtrusionPathSloped sp;
            deserialise_path_fields(sj, sp);
            sp.slope_begin.z_ratio = sj.at("slope_begin").at("z_ratio").get<double>();
            sp.slope_begin.e_ratio = sj.at("slope_begin").at("e_ratio").get<double>();
            sp.slope_end.z_ratio   = sj.at("slope_end").at("z_ratio").get<double>();
            sp.slope_end.e_ratio   = sj.at("slope_end").at("e_ratio").get<double>();
            loop->starts.push_back(std::move(sp));
        }

        // Sloped ends
        for (const auto &ej : j.at("ends")) {
            ExtrusionPathSloped sp;
            deserialise_path_fields(ej, sp);
            sp.slope_begin.z_ratio = ej.at("slope_begin").at("z_ratio").get<double>();
            sp.slope_begin.e_ratio = ej.at("slope_begin").at("e_ratio").get<double>();
            sp.slope_end.z_ratio   = ej.at("slope_end").at("z_ratio").get<double>();
            sp.slope_end.e_ratio   = ej.at("slope_end").at("e_ratio").get<double>();
            loop->ends.push_back(std::move(sp));
        }

        return loop;
    }

    // --- loop ---
    if (type == "loop") {
        auto *loop = new ExtrusionLoop();
        loop->set_loop_role(string_to_loop_role(
            j.at("loop_role").get<std::string>()));
        loop->print_after_infill = j.at("print_after_infill").get<bool>();
        loop->inset_idx          = j.at("inset_idx").get<int>();

        for (const auto &pj : j.at("paths")) {
            ExtrusionPath p;
            deserialise_path_fields(pj, p);
            loop->paths.push_back(std::move(p));
        }

        return loop;
    }

    // --- multipath ---
    if (type == "multipath") {
        auto *mp = new ExtrusionMultiPath();
        mp->set_can_reverse(j.at("can_reverse").get<bool>());
        mp->inset_idx = j.at("inset_idx").get<int>();

        for (const auto &pj : j.at("paths")) {
            ExtrusionPath p;
            deserialise_path_fields(pj, p);
            mp->paths.push_back(std::move(p));

            // Per-sub-path annotations: annotate via the multipath pointer
        }

        return mp;
    }

    // --- path_sloped ---
    if (type == "path_sloped") {
        auto *ps = new ExtrusionPathSloped();
        deserialise_path_fields(j, *ps);
        ps->slope_begin.z_ratio = j.at("slope_begin").at("z_ratio").get<double>();
        ps->slope_begin.e_ratio = j.at("slope_begin").at("e_ratio").get<double>();
        ps->slope_end.z_ratio   = j.at("slope_end").at("z_ratio").get<double>();
        ps->slope_end.e_ratio   = j.at("slope_end").at("e_ratio").get<double>();

        MaterialAnnotation ann;
        if (extract_annotation(j, ann))
            annotations[ps] = std::move(ann);

        return ps;
    }

    // --- path_contoured ---
    if (type == "path_contoured") {
        auto *pc = new ExtrusionPathContoured();
        deserialise_path_fields(j, *pc);

        for (const auto &zd : j.at("z_diffs"))
            pc->z_diffs.push_back(zd.get<double>());

        MaterialAnnotation ann;
        if (extract_annotation(j, ann))
            annotations[pc] = std::move(ann);

        return pc;
    }

    // --- path_oriented ---
    if (type == "path_oriented") {
        auto *po = new ExtrusionPathOriented();
        deserialise_path_fields(j, *po);

        MaterialAnnotation ann;
        if (extract_annotation(j, ann))
            annotations[po] = std::move(ann);

        return po;
    }

    // --- path (base leaf) ---
    if (type == "path") {
        auto *p = new ExtrusionPath();
        deserialise_path_fields(j, *p);

        MaterialAnnotation ann;
        if (extract_annotation(j, ann))
            annotations[p] = std::move(ann);

        return p;
    }

    throw Slic3r::RuntimeError(
        "ToolpathIR import: unrecognised entity type '" + type + "'");
}

static void deserialise_collection(const json &j,
                                   ExtrusionEntityCollection &collection,
                                   MaterialAnnotationMap &annotations)
{
    assert(j.at("type").get<std::string>() == "collection");

    collection.no_sort = j.at("no_sort").get<bool>();

    for (const auto &ej : j.at("entities")) {
        ExtrusionEntity *entity = deserialise_entity(ej, annotations);
        collection.entities.push_back(entity);
    }
}

// =========================================================================
// import_toolpath_ir (§5.1, §5.2, §5.3)
// =========================================================================

bool import_toolpath_ir(
    std::vector<PrintObject*>  &objects,
    const std::string          &input_path,
    MaterialAnnotationMap      &annotations_out)
{
    // Read and parse the IR file
    std::ifstream ifs(input_path);
    if (!ifs.is_open())
        return false;

    json root;
    try {
        root = json::parse(ifs);
    } catch (const json::parse_error &e) {
        throw Slic3r::RuntimeError(
            std::string("ToolpathIR import: JSON parse error — ") + e.what());
    }

    // Validate header
    const int version = root.at("version").get<int>();
    if (version != 1)
        throw Slic3r::RuntimeError(
            "ToolpathIR import: unsupported IR version " + std::to_string(version));

    annotations_out.clear();

    // Process each object block
    for (const auto &j_obj : root.at("objects")) {
        const int obj_id = j_obj.at("object_id").get<int>();
        if (obj_id < 0 || obj_id >= static_cast<int>(objects.size()))
            throw Slic3r::RuntimeError(
                "ToolpathIR import: object_id " + std::to_string(obj_id) +
                " out of range (have " + std::to_string(objects.size()) + " objects)");

        PrintObject *obj = objects[obj_id];

        // Build layer lookup by id for O(1) matching
        std::unordered_map<int, Layer*> layer_map;
        for (Layer *layer : obj->layers())
            layer_map[static_cast<int>(layer->id())] = layer;

        std::unordered_map<int, SupportLayer*> support_map;
        for (SupportLayer *sl : obj->support_layers())
            support_map[static_cast<int>(sl->id())] = sl;

        // ------ Object layers ------
        for (const auto &j_layer : j_obj.at("layers")) {
            const int layer_id = j_layer.at("layer_id").get<int>();
            auto it = layer_map.find(layer_id);
            if (it == layer_map.end())
                throw Slic3r::RuntimeError(
                    "ToolpathIR import: layer_id " + std::to_string(layer_id) +
                    " not found in object " + std::to_string(obj_id));
            Layer *layer = it->second;

            for (const auto &j_region : j_layer.at("regions")) {
                const int region_idx = j_region.at("region_idx").get<int>();
                if (region_idx < 0 ||
                    region_idx >= static_cast<int>(layer->region_count()))
                    throw Slic3r::RuntimeError(
                        "ToolpathIR import: region_idx " +
                        std::to_string(region_idx) + " out of range");

                LayerRegion *region = layer->get_region(region_idx);

                // Clear existing collections and rebuild (§5.3)
                region->perimeters.clear();
                deserialise_collection(j_region.at("perimeters"),
                                       region->perimeters, annotations_out);

                region->fills.clear();
                deserialise_collection(j_region.at("fills"),
                                       region->fills, annotations_out);

                region->thin_fills.clear();
                deserialise_collection(j_region.at("thin_fills"),
                                       region->thin_fills, annotations_out);
            }
        }

        // ------ Support layers ------
        for (const auto &j_sl : j_obj.at("support_layers")) {
            const int sl_id = j_sl.at("layer_id").get<int>();
            auto it = support_map.find(sl_id);
            if (it == support_map.end())
                throw Slic3r::RuntimeError(
                    "ToolpathIR import: support layer_id " +
                    std::to_string(sl_id) + " not found in object " +
                    std::to_string(obj_id));

            SupportLayer *sl = it->second;
            sl->support_fills.clear();
            deserialise_collection(j_sl.at("support_fills"),
                                   sl->support_fills, annotations_out);
        }
    }

    return true;
}

} // namespace Slic3r

