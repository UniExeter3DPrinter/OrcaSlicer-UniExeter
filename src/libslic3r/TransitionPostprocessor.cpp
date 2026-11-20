
// TransitionPostprocessor.cpp — Subprocess invocation implementation
//
// Spec reference: ORCASLICER_IR_PLUGIN.md §8
//
// CORRECTED: Uses DynamicPrintConfig::opt_*() accessors instead of
// direct member access. All config reads go through the string-keyed
// runtime lookup.

#include "TransitionPostprocessor.hpp"
#include "ToolpathIR.hpp"
#include "PrintConfig.hpp"
#include "Print.hpp"
#include "libslic3r.h"

#include <boost/process.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#ifdef _WIN32
#include <boost/process/windows.hpp>
#endif

#include <chrono>
#include <sstream>
#include <stdexcept>

namespace bp = boost::process;
namespace bf = boost::filesystem;

namespace Slic3r {

// =========================================================================
// run_transition_postprocessor (§8.2, §8.3)
// =========================================================================

TransitionPostprocessorResult run_transition_postprocessor(
    const std::string  &postprocessor_path,
    const std::string  &input_ir_path,
    const std::string  &output_ir_path,
    const std::string  &transition_db_path,
    const std::string  &sidecar_path,
    double              pellet_length,
    double              bed_width,
    double              bed_depth,
    const std::string  &transition_mode,
    int                 timeout_seconds)
{
    TransitionPostprocessorResult result;

    // ---- Validate executable exists (§8.5 row 1) ----
    if (!bf::exists(postprocessor_path)) {
        result.success       = false;
        result.exit_code     = -1;
        result.stderr_output = "Transition postprocessor executable not found: "
                             + postprocessor_path;
        return result;
    }

    // ---- Validate input IR exists ----
    if (!bf::exists(input_ir_path)) {
        result.success       = false;
        result.exit_code     = -1;
        result.stderr_output = "Toolpath IR file not found: " + input_ir_path;
        return result;
    }

    // ---- Detect .py scripts that need the Python interpreter ----
    // The production postprocessor is a native executable, but during
    // development a Python stub may be used.  boost::process passes
    // postprocessor_path as lpApplicationName to CreateProcess(), which
    // only executes PE binaries (.exe/.com).
    //
    // We build a single command string and launch via bp::shell (i.e.
    // cmd.exe /c) so that PATH resolution, .py association, and .bat
    // execution all work natively.  We intentionally avoid
    // bp::search_path to prevent pulling in extra Boost DLL dependencies
    // that may not be present in the build tree.

    const bool is_python_script =
        postprocessor_path.size() >= 3
        && postprocessor_path.compare(
               postprocessor_path.size() - 3, 3, ".py") == 0;

    // ---- Build command string ----
    std::string cmd_line;
    if (is_python_script) {
        BOOST_LOG_TRIVIAL(info)
            << "Transition postprocessor: detected .py script, "
               "launching via python interpreter";
        cmd_line = "python \"" + postprocessor_path + "\"";
    } else {
        cmd_line = "\"" + postprocessor_path + "\"";
    }

    cmd_line += " --input \""          + input_ir_path      + "\"";
    cmd_line += " --output \""         + output_ir_path     + "\"";
    cmd_line += " --database \""       + transition_db_path + "\"";
    cmd_line += " --sidecar \""        + sidecar_path + "\"";
    cmd_line += " --pellet-length "    + std::to_string(pellet_length);
    cmd_line += " --bed_size "         + std::to_string(bed_width) + "," + std::to_string(bed_depth);
    cmd_line += " --mode "             + transition_mode;

    BOOST_LOG_TRIVIAL(info)
        << "Transition postprocessor: command: " << cmd_line;

    // ---- Launch via shell (§8.3) ----
    bp::ipstream stderr_stream;
    bp::ipstream stdout_stream;
    auto start = std::chrono::steady_clock::now();

    try {
#ifdef _WIN32
        bp::child proc(
            cmd_line,
            bp::shell,
            ::boost::process::windows::create_no_window,
            bp::std_err > stderr_stream,
            bp::std_out > stdout_stream
        );
#else
        bp::child proc(
            cmd_line,
            bp::shell,
            bp::std_err > stderr_stream,
            bp::std_out > stdout_stream
        );
#endif

        // ---- Wait with timeout (§8.5 row 3) ----
        if (timeout_seconds > 0) {
            if (!proc.wait_for(std::chrono::seconds(timeout_seconds))) {
                proc.terminate();
                proc.wait();

                auto end = std::chrono::steady_clock::now();
                result.elapsed_secs = std::chrono::duration<double>(
                    end - start).count();
                result.success      = false;
                result.exit_code    = -1;
                result.stderr_output =
                    "Transition postprocessor timed out after "
                    + std::to_string(timeout_seconds) + " seconds";
                return result;
            }
        } else {
            proc.wait();
        }

        // ---- Relay postprocessor stdout to BOOST_LOG ----
        {
            std::string line;
            while (std::getline(stdout_stream, line)) {
                // Strip trailing \r on Windows
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (!line.empty())
                    BOOST_LOG_TRIVIAL(info) << line;
            }
        }

        auto end = std::chrono::steady_clock::now();
        result.elapsed_secs = std::chrono::duration<double>(
            end - start).count();
        result.exit_code    = proc.exit_code();

        std::ostringstream stderr_buf;
        stderr_buf << stderr_stream.rdbuf();
        result.stderr_output = stderr_buf.str();

        result.success = (result.exit_code == 0);

    } catch (const bp::process_error &e) {
        auto end = std::chrono::steady_clock::now();
        result.elapsed_secs  = std::chrono::duration<double>(
            end - start).count();
        result.success       = false;
        result.exit_code     = -1;
        result.stderr_output = std::string("Failed to launch postprocessor: ")
                             + e.what();
    }

    return result;
}


// =========================================================================
// run_transition_pipeline (§8.4)
//
// CORRECTED: reads config via DynamicPrintConfig::opt_string() etc.
// =========================================================================

void run_transition_pipeline(
    std::vector<PrintObject*>      &objects,
    const DynamicPrintConfig       &config,
    const std::string              &temp_dir,
    MaterialAnnotationMap          &annotations_out)
{
    // ---- Read config keys via runtime accessors ----
    const std::string postprocessor_path =
        config.opt_string("transition_postprocessor_path");
    const std::string database_path =
        config.opt_string("transition_database_path");
    const std::string sidecar_path =
        config.opt_string("transition_sidecar_path");
    const double pellet_len =
        config.opt_float("transition_pellet_length");
    const double bed_width =
        config.opt_float("transition_bed_width");
    const double bed_depth =
        config.opt_float("transition_bed_depth");
    const std::string mode =
        config.opt_string("transition_mode");
    const int timeout =
        config.opt_int("transition_postprocessor_timeout");

    // ---- Build temp file paths ----
    const std::string ir_export_path =
        (bf::path(temp_dir) / "toolpath_ir.json").string();
    const std::string ir_import_path =
        (bf::path(temp_dir) / "toolpath_ir_modified.json").string();

    // ---- Step 1: Export (§8.4 step 1) ----
    BOOST_LOG_TRIVIAL(info)
        << "Transition pipeline: exporting toolpath IR to "
        << ir_export_path;

    if (!export_toolpath_ir(objects, ir_export_path)) {
        throw Slic3r::RuntimeError(
            "Toolpath IR export failed — could not write " + ir_export_path);
    }

    // ---- Step 2: Run postprocessor (§8.4 step 2) ----
    BOOST_LOG_TRIVIAL(info)
        << "Transition pipeline: launching postprocessor "
        << postprocessor_path
        << " (pellet_length=" << pellet_len << " mm, bed_width=" << bed_width << " mm, bed_depth=" << bed_depth << " mm)";

    auto result = run_transition_postprocessor(
        postprocessor_path,
        ir_export_path,
        ir_import_path,
        database_path,
        sidecar_path,
        pellet_len,
        bed_width,
        bed_depth,
        mode,
        timeout
    );

    if (!result.success) {
        bf::remove(ir_export_path);

        std::string msg = "Transition postprocessor failed";
        if (result.exit_code >= 0)
            msg += " (exit code " + std::to_string(result.exit_code) + ")";
        if (!result.stderr_output.empty())
            msg += ": " + result.stderr_output;
        throw Slic3r::RuntimeError(msg);
    }

    BOOST_LOG_TRIVIAL(info)
        << "Transition pipeline: postprocessor completed in "
        << result.elapsed_secs << "s";

    // ---- Step 3: Verify output exists (§8.5 row 4) ----
    if (!bf::exists(ir_import_path)) {
        bf::remove(ir_export_path);
        throw Slic3r::RuntimeError(
            "Transition postprocessor did not produce output file: "
            + ir_import_path);
    }

    // ---- Step 4: Import (§8.4 step 3) ----
    BOOST_LOG_TRIVIAL(info)
        << "Transition pipeline: importing modified IR from "
        << ir_import_path;

    annotations_out.clear();

    if (!import_toolpath_ir(objects, ir_import_path, annotations_out)) {
        bf::remove(ir_export_path);
        bf::remove(ir_import_path);
        throw Slic3r::RuntimeError(
            "Toolpath IR import failed — could not parse " + ir_import_path);
    }

    BOOST_LOG_TRIVIAL(info)
        << "Transition pipeline: imported "
        << annotations_out.size() << " material annotations";

    // ---- Step 5: Cleanup temp files (§8.4 step 4) ----
    BOOST_LOG_TRIVIAL(info)
        << "Transition pipeline: cleanup — removing " << ir_export_path;
    bf::remove(ir_export_path);

    BOOST_LOG_TRIVIAL(info)
        << "Transition pipeline: cleanup — removing " << ir_import_path;
    bf::remove(ir_import_path);
}

} // namespace Slic3r

