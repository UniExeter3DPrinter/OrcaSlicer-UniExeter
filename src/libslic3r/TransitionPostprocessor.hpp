
#ifndef slic3r_TransitionPostprocessor_hpp_
#define slic3r_TransitionPostprocessor_hpp_

// TransitionPostprocessor.hpp — Subprocess invocation for external postprocessor
//
// Spec reference: ORCASLICER_IR_PLUGIN.md §8
//
// Calls the external transition postprocessor as a child process, passing
// the exported IR file and receiving the modified IR file back.
// The postprocessor is a standalone executable — no shared memory, no
// dynamic linking, no ABI coupling (§1).

#include <string>
#include <vector>

namespace Slic3r {

// Forward declarations
class PrintObject;
class PrintConfig;
struct MaterialAnnotationMap;

// -------------------------------------------------------------------------
// Result struct (§8.2)
// -------------------------------------------------------------------------

struct TransitionPostprocessorResult {
    bool        success      = false;
    int         exit_code    = -1;
    std::string stderr_output;
    double      elapsed_secs = 0.0;   // wall-clock time for logging
};

// -------------------------------------------------------------------------
// Subprocess invocation (§8.2, §8.3)
// -------------------------------------------------------------------------
// Launches the postprocessor executable as a child process and waits for
// completion (blocking). The postprocessor reads input_ir_path and writes
// output_ir_path.
//
// Returns a result struct with success/failure status, exit code, and any
// stderr output captured from the child process.

TransitionPostprocessorResult run_transition_postprocessor(
    const std::string  &postprocessor_path,
    const std::string  &input_ir_path,
    const std::string  &output_ir_path,
    const std::string  &transition_db_path,
    const std::string  &transition_mode,
    int                 timeout_seconds
);

// -------------------------------------------------------------------------
// Full pipeline orchestration (§8.4)
// -------------------------------------------------------------------------
// Convenience function that runs the complete export → subprocess → import
// sequence. Called from Print::export_gcode() (§3.2).
//
// Steps:
//   1. Export IR from PrintObject data
//   2. Launch postprocessor subprocess
//   3. Import modified IR back into PrintObject data
//   4. Populate annotation map for GCode consumption
//   5. Clean up temp files
//
// Throws Slic3r::RuntimeError on any failure (§8.5).

void run_transition_pipeline(
    std::vector<PrintObject*>  &objects,
    const PrintConfig          &config,
    const std::string          &temp_dir,
    MaterialAnnotationMap      &annotations_out
);

} // namespace Slic3r

#endif // slic3r_TransitionPostprocessor_hpp_

