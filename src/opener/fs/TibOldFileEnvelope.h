#pragma once

#include <cstddef>
#include <cstdint>

namespace peare {
namespace fs {

enum class TibOldEnvelopeStatus {
    NeedMore,
    Complete,
    InvalidArgument,
    InvalidVersion,
    Overflow,
    Truncated,
    MissingPrimary,
    CallbackStopped
};

class TibOldEnvelopeSink {
public:
    virtual ~TibOldEnvelopeSink() {}
    virtual bool primaryLength(std::uint64_t length) = 0;
    virtual bool primaryData(const std::uint8_t* data, std::size_t size) = 0;
};

// The sink must not throw. Length and data remain tentative until the outer
// field checksum is verified; Complete validates only this envelope's framing.
// Data after the first primary stream is ignored, including in later calls.
class TibOldFileEnvelope {
public:
    explicit TibOldFileEnvelope(TibOldEnvelopeSink& sink);

    TibOldEnvelopeStatus push(const std::uint8_t* data, std::size_t size);
    TibOldEnvelopeStatus finish();

private:
    enum class State { EnvelopeHeader, Metadata, StreamHeader, SkipStream, Primary, Done, Failed };

    TibOldEnvelopeStatus fail(TibOldEnvelopeStatus status);

    TibOldEnvelopeSink& sink_;
    State state_;
    TibOldEnvelopeStatus failure_;
    std::uint8_t header_[20];
    std::size_t headerSize_;
    std::uint64_t remaining_;
    std::uint64_t offset_;
};

} // namespace fs
} // namespace peare
