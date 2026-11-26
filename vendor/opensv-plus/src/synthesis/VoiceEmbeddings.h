#pragma once
#include <juce_core/juce_core.h>
#include <vector>
#include <map>
#include <string>

namespace sv::synthesis
{
// Split model files are shared across singers. These NOFS values override
// serialized defaults; absent fields retain legacy embedded model behavior.
struct VoiceEmbeddings
{
    std::vector<float> acousticSpeaker;
    std::vector<float> acousticStyle;
    std::vector<float> pitchSpeaker;
    std::vector<float> durationVoice;
    std::map<std::string, std::vector<float>> vocalModes;
    [[nodiscard]] juce::Result load(const juce::var& document);
};
}
