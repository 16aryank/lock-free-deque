# Lock Free Work-Stealing Deque
An implementation of a Chase-Lev Dynamic Circular Work-Stealing Deque. The implementation is based on [this](https://www.dre.vanderbilt.edu/~schmidt/PDF/work-stealing-dequeue.pdf) paper.

## Overview

The general algorithm is described in the above paper. The deque follows the paper exactly. I implemented this for the following reasons:

1. Learn about atomicity, specifically the `std::atomic` library, and learn about lock-free data structures.
2. Learn how to implement data structures / algorithms based on academic papers, since I've never done that before. In the process of implementing the deque, I also read a paper on the ABA problem and learned a few ways how to implement them.
3. Learn more about C++ features. Beyond `std::atomic`, I learned about `std::enable_shared_from_this`, `std::hazard_pointer` (although I did not use them since my toolchain does not yet support C++26), and the `explicit` keyword.

Below the underlying data structures for the deque are described. 

## Circular Array

The implementation of the array is stored in `circular_array.h`. The array has five private data members: the log of its size, `log_size_`; a unique pointer to the underlying array of variables, `segment_`; a low water mark, `low_water_mark_`; a shared pointer to the previous circular array, `prev_`; and an atomic pointer to the next pool, `pool_next_`.

The low-water mark is used as an optimization while shrinking. The previous, smaller array will still contain a lot of the same valid data, so you will only need to copy over the delta of elements that were added while the bigger array was active. As Chase and Lev describe:

> When a deque shrinks its array, only the elements stored in indexes greater than or equal to the low water mark of the bigger array are copied

To store the previous array, the Circular Array class publically inherits from `std::enable_shared_from_this`. Without it, the getting the previous array via a new shared pointer would create two independent control blocks for the same object, leading to double deletion and null pointer accesses. Instead, the `std::shared_from_this` allows the array to safely generate additional shared pointer instances of the same ownership.

The array uses a shared pool to store the arrays after they’re no longer reachable by any deque or in‑flight stealer. This happens when the last shared_ptr reference to that array goes away. The pool reuses arrays via the custom deleter, so the arrays are still eventually recycled. The pool is implemented via a vector of unique pointer of Trieber Stacks of the Circular Array, where the index of the vector represents the log size of the Circular Array. 

## Treiber Stack

Treiber Stack's work by appending an element to the top of the stack _iff_ that element is guaranteed, via a CAS (weak) loop, to be the only element to be the added since the operation began. Since C++ does not have garbage collection, tagged pointers are used to sovle the ABA problem. 