#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace studiocast::open_cuda {

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

struct ModelPack {
  // Schema v1
  std::string id;
  std::string display_name;
  std::string task;  // v1: "matting"
  std::string onnx_filename;

  ModelTensorSpec input;
  ModelOutputSpec output;
  ModelPreprocessSpec preprocess;

  // Derived from install layout
  std::filesystem::path root_dir;
  std::filesystem::path manifest_path;
  std::filesystem::path onnx_path;
  std::optional<std::filesystem::path> license_path;
};

// Registry for model packs under:
//   <models_root>/open_cuda/<model_id>/
// where <models_root> is normally ~/.local/share/studiocast/models.
class ModelPackRegistry {
 public:
  // Scan the given Open CUDA models directory (the directory containing <model_id>/...).
  //
  // Any pack that fails to load/validate is recorded in Problems() with a reason string.
  // Valid packs are available via ListModels()/ResolveModel().
  static ModelPackRegistry Scan(const std::filesystem::path& open_cuda_models_dir);

  // Convenience for scanning the default XDG location.
  static ModelPackRegistry ScanDefault();

  const std::vector<ModelPack>& ListModels() const { return models_; }
  std::optional<ModelPack> ResolveModel(const std::string& id) const;

  // Deterministic default selection (configurable later).
  // Current behavior:
  //  - Prefer "modnet" when present.
  //  - Else return first installed model ID (sorted).
  //  - Else return empty string.
  std::string DefaultModelId() const;

  // Key is best-effort model id; if unknown, the directory name is used.
  const std::map<std::string, std::string>& Problems() const { return problems_; }

 private:
  std::filesystem::path root_;
  std::vector<ModelPack> models_;  // sorted by id
  std::map<std::string, std::string> problems_;  // sorted by key
};

}  // namespace studiocast::open_cuda
