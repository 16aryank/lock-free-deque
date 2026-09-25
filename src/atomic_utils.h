#pragma once

#include <atomic>
#include <type_traits>

template <class T>
concept LockFreeAtomicValue =
    std::is_trivially_copyable_v<T> &&
    std::is_same_v<T, std::remove_cv_t<T>> &&
    std::is_trivially_copy_constructible_v<T> &&
    std::is_trivially_move_constructible_v<T> &&
    std::is_trivially_copy_assignable_v<T> &&
    std::is_trivially_move_assignable_v<T> &&
    std::atomic<T>::is_always_lock_free;
