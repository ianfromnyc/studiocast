#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "core/open_audio/model_pack_registry.h"
#include "core/open_video/model_pack_registry.h"

namespace {

bool Expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << message << "\n";
    return false;
  }
  return true;
}

bool ExpectEq(const std::string &name, const std::string &got,
              const std::string &want) {
  if (got == want)
    return true;
  std::cerr << name << "\n  got:  " << got << "\n  want: " << want << "\n";
  return false;
}

template <typename Result>
const Result *FindResult(const std::vector<Result> &results,
                         const std::string &id) {
  for (const auto &r : results) {
    if (r.id == id)
      return &r;
  }
  return nullptr;
}

bool TestOpenVideoIntegrity() {
  namespace ov = studiocast::open_video;
  const auto root = std::filesystem::path("tests") / "data" / "models" /
                    "model_integrity" / "open_video";

  const auto reg = ov::ModelPackRegistry::Scan(root);
  if (!Expect(reg.ResolveModel("video_good_sha").has_value(),
              "video_good_sha should scan as usable"))
    return false;
  if (!Expect(reg.ResolveModel("video_placeholder_pack") == std::nullopt,
              "placeholder Open Video pack should not be usable"))
    return false;
  {
    const auto &problems = reg.Problems();
    auto it = problems.find("video_placeholder_pack");
    if (!Expect(it != problems.end(),
                "placeholder Open Video pack should be reported"))
      return false;
    if (!Expect(it->second.find("placeholder") != std::string::npos,
                "placeholder Open Video problem should be classified"))
      return false;
  }

  const auto results = ov::ModelPackRegistry::Verify(root);
  const auto *good = FindResult(results, "video_good_sha");
  if (!Expect(good && good->ok, "video_good_sha should verify"))
    return false;
  if (!Expect(!good->files.empty(), "video_good_sha should include file result"))
    return false;
  if (!ExpectEq("video_good_sha status", good->status, "ok"))
    return false;
  if (!ExpectEq("video_good_sha file actual", good->files.front().actual_sha256,
                "cbc724d038823203ec77f127b74e6d84341e15f2befee18af5d6f5739f00492b"))
    return false;
  if (!ExpectEq("video_good_sha checksum kind",
                good->files.front().checksum_kind, "installed_sha256"))
    return false;

  const auto *bad = FindResult(results, "video_bad_sha");
  if (!Expect(bad && !bad->ok, "video_bad_sha should fail verification"))
    return false;
  if (!ExpectEq("video_bad_sha status", bad->status, "checksum_mismatch"))
    return false;
  if (!Expect(!bad->files.empty() &&
                  bad->files.front().status == "checksum_mismatch",
              "video_bad_sha file should report checksum_mismatch"))
    return false;

  const auto *missing = FindResult(results, "video_missing_file");
  if (!Expect(missing && !missing->ok,
              "video_missing_file should fail verification"))
    return false;
  if (!ExpectEq("video_missing_file status", missing->status, "missing"))
    return false;

  const auto *invalid = FindResult(results, "video_invalid_manifest");
  if (!Expect(invalid && !invalid->ok,
              "video_invalid_manifest should fail verification"))
    return false;
  if (!ExpectEq("video_invalid_manifest status", invalid->status,
                "invalid_manifest"))
    return false;

  const auto *placeholder = FindResult(results, "video_placeholder_pack");
  if (!Expect(placeholder && !placeholder->ok,
              "video_placeholder_pack should fail verification"))
    return false;
  return ExpectEq("video_placeholder_pack status", placeholder->status,
                  "placeholder");
}

bool TestOpenAudioIntegrity() {
  namespace oa = studiocast::open_audio;
  const auto root = std::filesystem::path("tests") / "data" / "models" /
                    "model_integrity" / "open_audio";

  const auto reg = oa::ModelPackRegistry::Scan(root);
  const auto goodPack = reg.ResolveModel("audio_good_sha");
  if (!Expect(goodPack.has_value(), "audio_good_sha should scan as usable"))
    return false;
  if (!ExpectEq("audio_good_sha origin_sha256", goodPack->origin_sha256,
                "0fb0fbc0fc5b6c4ddff835be62dfaa50e204ad86b993a4c4695eee5fba8e5c2a"))
    return false;
  if (!Expect(reg.ResolveModel("audio_placeholder_pack") == std::nullopt,
              "placeholder Open Audio pack should not be usable"))
    return false;

  const auto results = oa::ModelPackRegistry::Verify(root);
  const auto *good = FindResult(results, "audio_good_sha");
  if (!Expect(good && good->ok, "audio_good_sha should verify"))
    return false;
  if (!Expect(!good->files.empty(), "audio_good_sha should include file result"))
    return false;
  if (!ExpectEq("audio_good_sha expected", good->files.front().expected_sha256,
                "0fb0fbc0fc5b6c4ddff835be62dfaa50e204ad86b993a4c4695eee5fba8e5c2a"))
    return false;
  if (!ExpectEq("audio_good_sha actual", good->files.front().actual_sha256,
                "0fb0fbc0fc5b6c4ddff835be62dfaa50e204ad86b993a4c4695eee5fba8e5c2a"))
    return false;
  if (!ExpectEq("audio_good_sha checksum kind",
                good->files.front().checksum_kind, "installed_sha256"))
    return false;

  const auto *bad = FindResult(results, "audio_bad_sha");
  if (!Expect(bad && !bad->ok, "audio_bad_sha should fail verification"))
    return false;
  if (!ExpectEq("audio_bad_sha status", bad->status, "checksum_mismatch"))
    return false;

  const auto *missing = FindResult(results, "audio_missing_file");
  if (!Expect(missing && !missing->ok,
              "audio_missing_file should fail verification"))
    return false;
  if (!ExpectEq("audio_missing_file status", missing->status, "missing"))
    return false;

  const auto *invalid = FindResult(results, "audio_invalid_manifest");
  if (!Expect(invalid && !invalid->ok,
              "audio_invalid_manifest should fail verification"))
    return false;
  if (!ExpectEq("audio_invalid_manifest status", invalid->status,
                "invalid_manifest"))
    return false;

  const auto *placeholder = FindResult(results, "audio_placeholder_pack");
  if (!Expect(placeholder && !placeholder->ok,
              "audio_placeholder_pack should fail verification"))
    return false;
  return ExpectEq("audio_placeholder_pack status", placeholder->status,
                  "placeholder");
}

} // namespace

int main() {
  bool ok = true;
  ok = TestOpenVideoIntegrity() && ok;
  ok = TestOpenAudioIntegrity() && ok;
  if (!ok)
    return 1;
  std::cout << "MODEL REGISTRY TESTS OK\n";
  return 0;
}
