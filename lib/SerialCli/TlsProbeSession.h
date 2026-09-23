// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef TLS_PROBE_SESSION_H
#define TLS_PROBE_SESSION_H

#include <atomic>

// Only the worker completes a run; only the main loop starts and consumes it.
class TlsProbeSession {
public:
    enum class State { Idle, Running, Done, Failed, Timeout };

    bool start() {
        State expected = State::Idle;
        return state.compare_exchange_strong(expected, State::Running);
    }

    bool finish(State outcome) {
        if (outcome != State::Done && outcome != State::Failed &&
            outcome != State::Timeout) return false;
        State expected = State::Running;
        if (!state.compare_exchange_strong(expected, outcome)) return false;
        completion.store(true, std::memory_order_release);
        return true;
    }

    bool consume(State& outcome) {
        if (!completion.exchange(false, std::memory_order_acquire)) return false;
        outcome = state.load();
        state.store(State::Idle);
        return true;
    }

    State current() const { return state.load(); }

private:
    std::atomic<State> state{State::Idle};
    std::atomic<bool> completion{false};
};

#endif  // TLS_PROBE_SESSION_H
