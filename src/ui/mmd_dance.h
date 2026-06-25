#pragma once

#include <string>

namespace anyadance::ui {

// Parameters for one MMD dance export. Paths are UTF-8; the few that are optional
// are auto-detected when left empty.
struct MmdDanceConfig {
    std::string vmdPath;       // .vmd motion (required)
    std::string modelPath;     // .pmx/.pmd model (required)
    std::string blenderPath;   // blender.exe (auto-detected if empty)
    std::string mmdToolsPath;  // MMD Tools add-on dir (auto-detected if empty)
    float fps = 60.0f;
};

// Result of running the Blender solve. On success, solvedJson holds the parsed-
// ready document and solvedJsonPath points at the single output file written to
// the temp directory (reused/overwritten each export).
struct MmdExportResult {
    bool ok = false;
    std::string message;
    std::string solvedJson;
    std::string solvedJsonPath;
};

// Auto-detect helpers; return empty UTF-8 strings when nothing is found.
std::string DetectBlenderExe();
std::string DetectMmdToolsPath();

// Native open-file dialog. Returns an empty string when cancelled. The filter is
// a UTF-8 "Label|*.ext" pair (a single '|' separates label and pattern).
std::string OpenFileDialog(void* ownerHwnd, const char* title, const char* label, const char* pattern);

// Native pick-folder dialog. Returns an empty string when cancelled.
std::string OpenFolderDialog(void* ownerHwnd, const char* title);

// Native save-file dialog. Returns an empty string when cancelled. The filter is
// a UTF-8 "Label|*.ext" pair; defaultExt (without a dot, e.g. "nya") is appended
// when the user types a name with no extension.
std::string SaveFileDialog(void* ownerHwnd, const char* title, const char* label, const char* pattern, const char* defaultExt);

// Whole-file read/write that accepts UTF-8 paths (Windows paths may be non-ASCII).
// ReadFileUtf8 returns an empty string when the file cannot be read; WriteFileUtf8
// returns false on failure.
std::string ReadFileUtf8(const std::string& utf8Path);
bool WriteFileUtf8(const std::string& utf8Path, const std::string& contents);

// Run the bundled Blender export script for this config. Blocking; call it from a
// worker thread so the UI keeps rendering while Blender solves.
MmdExportResult RunMmdExport(const MmdDanceConfig& config);

} // namespace anyadance::ui
