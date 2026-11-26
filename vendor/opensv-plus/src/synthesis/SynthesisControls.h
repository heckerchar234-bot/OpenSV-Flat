#pragma once
#include <map>
#include <span>
#include <string>

namespace sv::synthesis {
struct SynthesisControls {
    std::span<const float> tension;
    std::span<const float> breathiness;
    std::span<const float> gender;
    std::span<const float> toneShift;
    std::span<const float> voicing;
    std::map<std::string, std::span<const float>> vocalModes;
};
}
