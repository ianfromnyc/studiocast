#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace studiocast::open_video {

struct ModelFile {
  // Original relative filename as declared in model.json.
  std::string name;

  // Kind of file (e.g., "onnx", "dlib_shape_predictor").
  std::string kind;

  // Optional role (e.g., "main", "left", "right").
  std::string role;

  // Optional sha256 (hex). Empty means unspecified.
  std::string sha256;

  // Resolved absolute path under the installed pack directory.
  std::filesystem::path path;
};

// v1-style ONNX tensor metadata.
// Used by a small subset of Open Video tasks (notably matting) where the runtime
// needs deterministic IO names and fixed model dimensions.
struct ModelTensorSpec {
  std::string name;
  std::string layout;   // e.g. "nchw" or "nhwc"
  std::string dtype;    // e.g. "float32"
  int width = 0;
  int height = 0;
  int channels = 0;
};

struct ModelOutputSpec {
  std::string name;
  std::string kind;   // e.g. "alpha"
  std::string dtype;  // e.g. "float32"
};

struct ModelPreprocessSpec {
  std::array<double, 3> mean{0.0, 0.0, 0.0};
  std::array<double, 3> std{1.0, 1.0, 1.0};
  std::string color;  // e.g. "rgb"
  std::string range;  // e.g. "0..1"
};

struct MattingSpec {
  ModelTensorSpec input;
  ModelOutputSpec output;
  ModelPreprocessSpec preprocess;
};

struct ModelPack {
  // Schema version (1 for legacy packs using onnx_filename; 2 for v2 packs).
  int schema_version = 1;

  std::string id;
  std::string display_name;
  std::string task;

  // Optional dependencies expressed as "<task>:<id>" strings.
  std::vector<std::string> depends_on;

  // Files declared by the pack. For schema v1 packs, this contains a single
  // ONNX file (role=main, kind=onnx).
  std::vector<ModelFile> files;

  // Optional task-specific metadata.
  // For task == "matting", this captures the v1-style IO/preprocess metadata.
  std::optional<MattingSpec> matting;

  // Derived from install layout.
  std::filesystem::path root_dir;
  std::filesystem::path manifest_path;
  std::optional<std::filesystem::path> license_path;
};

// Registry for model packs under:
//   <models_root>/open_video/<subject>/<pack_dir>/
// where <models_root> is normally ~/.local/share/studiocast/models.
class ModelPackRegistry {
 public:
  // Scan the given Open Video models directory (the directory containing <subject>/...).
  //
  // Any pack that fails to load/validate is recorded in Problems() with a reason string.
  // Valid packs are available via ListModels()/ResolveModel().
  static ModelPackRegistry Scan(const std::filesystem::path& open_video_models_dir);

  // Convenience for scanning the default XDG location.
  static ModelPackRegistry ScanDefault();

  const std::vector<ModelPack>& ListModels() const { return models_; }
  std::optional<ModelPack> ResolveModel(const std::string& id) const;

  // Convenience view of installed model ids by task.
  //
  // This is used by effect runtimes to pick a deterministic default and to
  // surface user-friendly install diagnostics.
  const std::map<std::string, std::vector<std::string>>& Tasks() const { return tasks_; }

  // Find a model pack by task + id.
  // Returns std::nullopt if the id is not found or does not match the task.
  std::optional<ModelPack> Find(const std::string& task, const std::string& id) const;

  // Deterministic default selection:
  //  - prefer first model that matches task
  //  - else first installed model
  //  - else empty
  std::string DefaultModelIdForTask(const std::string& task) const;

  // Key is best-effort model id; if unknown, the pack directory is used.
  const std::map<std::string, std::string>& Problems() const { return problems_; }

 private:
  std::filesystem::path root_;
  std::vector<ModelPack> models_;                // sorted by task,id
  std::map<std::string, std::string> problems_;  // sorted by key
  std::map<std::string, std::vector<std::string>> tasks_;  // task -> model ids
};

}  // namespace studiocast::open_video
