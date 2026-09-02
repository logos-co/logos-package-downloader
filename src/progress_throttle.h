#pragma once

#include <cstdint>

namespace lgpd {

/// Rate limiter for progress samples. libcurl fires per chunk — hundreds a
/// second on a fast link — and each one becomes a cross-process event.
/// Time is injected so the policy is testable without sleeping.
class ProgressThrottle {
public:
    explicit ProgressThrottle(std::uint64_t minIntervalMs = 200)
        : m_minIntervalMs(minIntervalMs) {}

    /// Passes the first sample and the completing one whatever the rate
    /// limit says; drops samples carrying no new bytes.
    bool shouldEmit(std::uint64_t received, std::uint64_t total, std::uint64_t nowMs);

private:
    std::uint64_t m_minIntervalMs;
    bool          m_emittedAny    = false;
    std::uint64_t m_lastEmitMs    = 0;
    std::uint64_t m_lastReceived  = 0;
};

} // namespace lgpd
