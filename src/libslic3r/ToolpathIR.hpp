
#ifndef slic3r_ToolpathIR_hpp_
#define slic3r_ToolpathIR_hpp_

// ToolpathIR.hpp — Toolpath Intermediate Representation export/import
//
// Spec reference: docs/ORCASLICER_IR_PLUGIN.md §4, §5, §6
//
// Serialises the ExtrusionEntity hierarchy to a JSON file (IR) for external
// postprocessing, and deserialises the (potentially modified) IR back into
// the PrintObject data structures. The annotation map populated during
// import carries material metadata through to G-code generation (§6).

#include <string>
#include <map>
#include <unordered_map>
#include <vector>

// Forward declarations — avoid pulling heavy headers into every TU
namespace Slic3r {
class PrintObject;
class ExtrusionEntity;
class ExtrusionPath;
enum  ExtrusionRole : uint8_t;
enum  ExtrusionLoopRole : uint8_t;  // elrDefault, elrHole, etc.
}

namespace Slic3r {

// -------------------------------------------------------------------------
// MaterialAnnotation — side-channel data from the postprocessor (§6.2)
// -------------------------------------------------------------------------
// Generic data container — stores whatever the IR extension fields contain.
// OrcaSlicer-side code does not interpret transition-specific semantics;
// those are documented in ORCASLICER_IR_TO_GCODE.md §2.

struct MaterialAnnotation {
    std::string                        material_id;
    std::map<std::string, double>      feed_state;           // material_id -> fraction
    bool                               is_transition       = false;
    double                             transition_progress = -1.0;  // 0.0–1.0, or -1 if N/A
    double                             nozzle_temp         = -1.0;  // °C, or -1 if N/A
};

// Keyed by raw ExtrusionEntity pointer — safe because entities are owned by
// ExtrusionEntityCollection which lives on PrintObject for the duration of
// G-code export (see §6.4 for the pointer stability guarantee).
using MaterialAnnotationMap =
    std::unordered_map<const ExtrusionEntity*, MaterialAnnotation>;

// -------------------------------------------------------------------------
// IR export (§4)
// -------------------------------------------------------------------------
// Serialises all objects' Layer/SupportLayer extrusion data to a JSON file.
// Coordinates are written as unscaled doubles (mm).
// Extension fields are written as null/false on export.
//
// Returns true on success.

bool export_toolpath_ir(
    const std::vector<PrintObject*> &objects,
    const std::string               &output_path
);

// -------------------------------------------------------------------------
// IR import (§5)
// -------------------------------------------------------------------------
// Deserialises the (possibly modified) IR file back into the PrintObject
// data structures, replacing existing ExtrusionEntityCollection contents.
// Populates the annotation map with any non-null extension field data.
//
// The caller (Print::export_gcode) passes the annotation map to GCode
// before calling do_export().
//
// Returns true on success.

bool import_toolpath_ir(
    std::vector<PrintObject*>  &objects,
    const std::string          &input_path,
    MaterialAnnotationMap      &annotations_out
);

// -------------------------------------------------------------------------
// ExtrusionRole <-> string conversion (§2.3)
// -------------------------------------------------------------------------
// Serialised as strings to survive enum reordering across OrcaSlicer versions.

const char* extrusion_role_to_string(ExtrusionRole role);
ExtrusionRole string_to_extrusion_role(const std::string &s);

// -------------------------------------------------------------------------
// ExtrusionLoopRole <-> string conversion (§10.6)
// -------------------------------------------------------------------------

const char* loop_role_to_string(ExtrusionLoopRole role);
ExtrusionLoopRole string_to_loop_role(const std::string &s);

} // namespace Slic3r

#endif // slic3r_ToolpathIR_hpp_

