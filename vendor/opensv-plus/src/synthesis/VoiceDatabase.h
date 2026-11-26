#pragma once

#include <juce_core/juce_core.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace sv::synthesis
{
struct VoiceEntry
{
    juce::MemoryBlock key;
    juce::String name;
    std::uint64_t valueOffset = 0;
    std::uint32_t valueSize = 0;
    // Non-empty when the value is a sibling model file instead of bytes inside
    // this container. Embedded entries keep an empty file and use valueOffset.
    juce::File external;
};

struct VoiceMetadata
{
    juce::String name;
    juce::String vendor;
    int version = -1;
    juce::String language;
    juce::String phoneset;
    juce::String type;
    juce::StringArray languages;
    juce::StringArray timbreStyles;
    juce::StringPairArray properties{false};
};

// File access belongs to one worker or message thread, never the audio callback.
// Entry references remain valid until the next call to open(). Binary keys are
// preserved verbatim; name is empty when a key is not printable UTF-8 text.
class VoiceDatabase
{
public:
    [[nodiscard]] juce::Result open(const juce::File& file, const juce::File& modelsRoot = {});
    [[nodiscard]] const std::vector<VoiceEntry>& getEntries() const noexcept;
    [[nodiscard]] const VoiceMetadata& getMetadata() const noexcept;
    [[nodiscard]] const juce::File& getFile() const noexcept;
    [[nodiscard]] const VoiceEntry* findEntry(juce::StringRef name) const;
    [[nodiscard]] juce::Result readEntry(const VoiceEntry& entry, juce::MemoryBlock& destination);

    // The current format stores configuration as JSON and keeps the models in
    // separate .dnni files. Absent for legacy embedded containers.
    [[nodiscard]] bool hasDocument() const noexcept;
    [[nodiscard]] const juce::var& getDocument() const noexcept;

private:
    [[nodiscard]] juce::Result readMetadata();
    [[nodiscard]] juce::Result openContainer(const juce::File& source, std::unique_ptr<juce::FileInputStream> input);
    [[nodiscard]] juce::Result openDocument(const juce::File& source);
    [[nodiscard]] juce::File locateModel(const juce::File& source, const juce::String& field, const juce::String& hash) const;

    juce::File file;
    juce::File modelDirectory;
    std::unique_ptr<juce::FileInputStream> stream;
    std::vector<VoiceEntry> entries;
    VoiceMetadata metadata;
    juce::var document;
};
} // namespace sv::synthesis
