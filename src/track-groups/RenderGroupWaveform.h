// Bounded streaming reduction of already-summed mono samples. The native mixer
// supplies samples; peaks are measured AFTER summing, preserving cancellation.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>
namespace TrackGroups {
struct WaveformColumn { float minimum{}, maximum{}; };
using MonoSampleReader = std::function<size_t(float *, size_t)>;
std::optional<std::vector<WaveformColumn>> RenderSummedWaveform(
   const MonoSampleReader &read, uint64_t frames, size_t columns,
   const std::function<bool()> &canceled = {});
}
