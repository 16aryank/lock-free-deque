#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

template <class Node>
class TreiberStack {
public:
    TreiberStack() = default;
    TreiberStack(const TreiberStack&) = delete;
    TreiberStack& operator=(const TreiberStack&) = delete;

    // Node needs at least 1 tag bit
    static_assert(alignof(Node) >= 2);

    void push(Node* node) {
        auto head = head_.load(std::memory_order_relaxed);
        do {
            node->pool_next_.store(unpack_ptr(head), std::memory_order_relaxed);
        } while (!head_.compare_exchange_weak(
            head,
            pack(node, next_tag(head)),
            std::memory_order_release,
            std::memory_order_relaxed));
    }

    Node* pop() {
        while (true) {
            auto head = head_.load(std::memory_order_acquire);
            auto* head_ptr = unpack_ptr(head);
            if (!head_ptr) {
                return nullptr;
            }

            Node* next = head_ptr->pool_next_.load(std::memory_order_relaxed);
            if (head_.compare_exchange_weak(
                    head,
                    next,
                    pack(next, next_tag(head)),
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                head_ptr->pool_next_.store(nullptr, std::memory_order_relaxed);
                return head_ptr;
            }
        }
    }

private:
    static constexpr std::uintptr_t tag_mask() {
        return alignof(Node) - 1;
    }

    static std::uintptr_t next_tag(std::uintptr_t packed) {
        return (packed + 1) & tag_mask();
    }

    static Node* unpack_ptr(std::uintptr_t packed) {
        return reinterpret_cast<Node*>(packed & ~tag_mask());
    }

    static std::uintptr_t pack(Node* ptr, std::uintptr_t tag) {
        return (reinterpret_cast<std::uintptr_t>(ptr) & ~tag_mask()) |
            (tag & tag_mask());
    }

    std::atomic<std::uintptr_t> head_{0};
};
