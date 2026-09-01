#include "progress_throttle.h"

namespace lgpd {

bool ProgressThrottle::shouldEmit(std::uint64_t received, std::uint64_t total,
                                  std::uint64_t nowMs) {
    // On a small file this may be the only sample.
    if (!m_emittedAny) {
        m_emittedAny   = true;
        m_lastEmitMs   = nowMs;
        m_lastReceived = received;
        return true;
    }
    // curl re-invokes on a timer as well as per chunk; drop the idle repeats.
    if (received <= m_lastReceived) return false;

    // Completion always passes, or the bar stalls just short of full.
    const bool complete = (total > 0 && received >= total);
    if (!complete && nowMs - m_lastEmitMs < m_minIntervalMs) return false;

    m_lastEmitMs   = nowMs;
    m_lastReceived = received;
    return true;
}

} // namespace lgpd
