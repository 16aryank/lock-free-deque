#pragma once

#include <optional>

enum class StealState {
    SUCCESS,
    EMPTY,
    ABORT,
};

template <class T>
struct StealResult {
    StealState state_;
    std::optional<T> value_;

    explicit StealResult<T>(StealState state) : 
        state_(state), value_(std::nullopt) {}

    explicit StealResult<T>(T value) :
        state_(StealState::SUCCESS), value_(value) {}
};

