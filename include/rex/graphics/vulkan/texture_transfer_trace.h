#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DECLARE(bool, vulkan_trace_texture_transfers);

namespace rex::graphics::vulkan {

// Metadata only: do not add readbacks or queue waits to a timing investigation.
// Keep only the first two occurrences of each transfer signature so routine
// per-frame resolves don't exhaust the budget before character generation.
// Shared across resolve/load sites. Counts persist until process exit.
inline uint32_t NextTextureTransferTraceId(const std::array<uint32_t, 8>& signature) {
  if (!REXCVAR_GET(vulkan_trace_texture_transfers)) {
    return 0;
  }
  static std::mutex mutex;
  static std::map<std::array<uint32_t, 8>, uint32_t> occurrences;
  static uint32_t event_count = 0;
  constexpr uint32_t kEventLimit = 8192;
  std::lock_guard lock(mutex);
  if (event_count == kEventLimit) {
    return 0;
  }
  auto it = occurrences.find(signature);
  if (it == occurrences.end()) {
    if (occurrences.size() == 4096) {
      return 0;
    }
    it = occurrences.emplace(signature, 0).first;
  }
  if (it->second == 2) {
    return 0;
  }
  ++it->second;
  if (++event_count == kEventLimit) {
    REXGPU_INFO("[vk-texture-trace] event budget reached; restart to capture more");
  }
  return event_count;
}

// Ensure every accepted request has an outcome, including early-return paths.
class TextureTransferTraceResult {
 public:
  TextureTransferTraceResult(uint32_t id, const char* stage) : id_(id), stage_(stage) {}
  ~TextureTransferTraceResult() {
    if (id_) {
      REXGPU_INFO("[vk-texture-trace] id={} stage={} outcome={}", id_, stage_, outcome_);
    }
  }
  void SetOutcome(const char* outcome) { outcome_ = outcome; }

 private:
  uint32_t id_;
  const char* stage_;
  const char* outcome_ = "early-return";
};

}  // namespace rex::graphics::vulkan
