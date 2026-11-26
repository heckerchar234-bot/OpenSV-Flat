#pragma once

#include "DnniReader.h"

#include <juce_core/juce_core.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace sv::synthesis
{
struct PhoneSet
{
    std::string name;
    std::vector<std::string> symbols;
    std::vector<std::string> categories;
    std::vector<std::string> unifiedSymbols;
    std::vector<std::string> classes;
};

// Copies and validates a _psv2 table; a failed read preserves the destination.
[[nodiscard]] juce::Result readPhoneSet(const DnniReader& reader, std::size_t nodeIndex, PhoneSet& output);
[[nodiscard]] bool resolvePhone(const PhoneSet& language, const PhoneSet& unified, std::string_view symbol,
                                std::size_t& targetIndex, std::string_view& category);
} // namespace sv::synthesis
