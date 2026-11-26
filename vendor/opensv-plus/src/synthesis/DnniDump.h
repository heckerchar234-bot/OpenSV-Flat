#pragma once

#include "DnniInference.h"

namespace sv::synthesis
{
namespace dnnidump
{
// Names the synthesis stage for the DNNI records written while this is alive.
// The name is thread local, so concurrently rendered tracks stay separable.
class Scope
{
public:
    explicit Scope(const char* name);
    ~Scope();

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    const char* previous = nullptr;
};

// Writes one frame-major tensor triplet per network call as raw little-endian
// float32 plus a JSON line in manifest.jsonl. A no-op unless the OPENSVDUMP
// environment variable names a directory, which it creates when absent.
void record(const DnniTensor& input, const DnniTensor* condition, const DnniTensor& output, std::size_t networkOffset);
} // namespace dnnidump
} // namespace sv::synthesis
