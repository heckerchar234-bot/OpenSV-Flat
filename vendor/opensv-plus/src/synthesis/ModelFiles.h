#pragma once

#include <juce_core/juce_core.h>

namespace sv::synthesis
{
inline bool isSafeModelId(const juce::String& id)
{
    // 与托管安装器保持一致：模型 ID 是任意安全文件名，不要求十六进制。
    if (id.trim().isEmpty() || id.length() > 240 || id == "." || id == ".." ||
        id.endsWithChar(' ') || id.endsWithChar('.') || id.containsAnyOf("<>:\"/\\|?*"))
    {
        return false;
    }
    auto characters = id.getCharPointer();
    while (!characters.isEmpty())
    {
        const auto character = characters.getAndAdvance();
        if (character < 0x20 || (character >= 0x7f && character <= 0x9f)) return false;
    }
    const auto device = id.upToFirstOccurrenceOf(".", false, false).trimCharactersAtEnd(" ").toUpperCase();
    if (device == "CON" || device == "PRN" || device == "AUX" || device == "NUL") return false;
    return !(device.length() == 4 && (device.startsWith("COM") || device.startsWith("LPT")) &&
        device[3] >= '1' && device[3] <= '9');
}

inline juce::File findModelFile(const juce::File& directory, const juce::String& id)
{
    const auto leaf = id + ".dnni";
    const auto expected = directory.getChildFile(leaf);
    if (expected.existsAsFile()) return expected;
    // Android 文件系统区分大小写，打包文件名可能与 NOFS 的大小写不同。
    for (const auto& file : directory.findChildFiles(juce::File::findFiles, false))
    {
        if (file.getFileName().equalsIgnoreCase(leaf)) return file;
    }
    return expected;
}
}
