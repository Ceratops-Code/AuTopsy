#include "RenderGroupWaveform.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
namespace TrackGroups {
std::optional<std::vector<WaveformColumn>> RenderSummedWaveform(
   const MonoSampleReader &read, uint64_t frames, size_t columns,
   const std::function<bool()> &canceled)
{
   if (!read || columns == 0 || columns > 65536 || frames > uint64_t{1} << 48)
      throw std::runtime_error("The group waveform has unusable dimensions.");
   std::vector<WaveformColumn> result(columns,
      {std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest()});
   std::array<float, 4096> buffer{};
   uint64_t offset{};
   while (offset < frames) {
      if (canceled && canceled()) return {};
      const auto wanted = size_t(std::min<uint64_t>(buffer.size(), frames - offset));
      const auto count = read(buffer.data(), wanted);
      if (count == 0 || count > wanted) throw std::runtime_error("The group mixer returned incomplete samples.");
      for (size_t index{}; index < count; ++index) {
         const auto value = buffer[index];
         if (!std::isfinite(value)) throw std::runtime_error("The group mixer returned a non-finite sample.");
         const auto column = size_t((offset + index) * columns / frames);
         result[column].minimum = std::min(result[column].minimum, value);
         result[column].maximum = std::max(result[column].maximum, value);
      }
      offset += count;
   }
   if (canceled && canceled()) return {};
   for (auto &column : result) if (column.minimum > column.maximum) column = {};
   return result;
}
}
