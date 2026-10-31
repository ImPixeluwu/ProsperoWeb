// SPDX-License-Identifier: GPL-3.0-or-later
//
// Force-included (-include) into every C++ translation unit of the WebKit
// build by scripts/ps5/build-webkit.sh.
//
// The payload SDK ships libc++ 18.1, which lacks std::atomic_ref (added in
// libc++ 19). WTF/Atomics.h uses it for all its atomic helpers. Provide a
// minimal implementation on top of the __atomic builtins. __cpp_lib_atomic_ref
// is deliberately left undefined so feature-testing code (simdutf) keeps its
// fallback.
#pragma once

#if defined(__cplusplus)
#include <version>
#endif

#if defined(__cplusplus) && defined(_LIBCPP_VERSION) && _LIBCPP_VERSION < 190000

#include <atomic>
#include <cstddef>
#include <type_traits>

_LIBCPP_BEGIN_NAMESPACE_STD

template<typename T>
class atomic_ref {
    static_assert(is_trivially_copyable_v<T>);

    static constexpr int toBuiltin(memory_order order)
    {
        switch (order) {
        case memory_order_relaxed: return __ATOMIC_RELAXED;
        case memory_order_consume: return __ATOMIC_CONSUME;
        case memory_order_acquire: return __ATOMIC_ACQUIRE;
        case memory_order_release: return __ATOMIC_RELEASE;
        case memory_order_acq_rel: return __ATOMIC_ACQ_REL;
        default: return __ATOMIC_SEQ_CST;
        }
    }

    static constexpr memory_order failureOrder(memory_order order)
    {
        if (order == memory_order_acq_rel)
            return memory_order_acquire;
        if (order == memory_order_release)
            return memory_order_relaxed;
        return order;
    }

    T* m_ptr;

public:
    using value_type = T;
    using difference_type = conditional_t<is_pointer_v<T>, ptrdiff_t, T>;

    static constexpr size_t required_alignment = (sizeof(T) & (sizeof(T) - 1)) || sizeof(T) > 16 ? alignof(T) : (sizeof(T) > alignof(T) ? sizeof(T) : alignof(T));
    static constexpr bool is_always_lock_free = __atomic_always_lock_free(sizeof(T), 0);

    explicit atomic_ref(T& object) : m_ptr(__builtin_addressof(object)) { }
    atomic_ref(const atomic_ref&) noexcept = default;
    atomic_ref& operator=(const atomic_ref&) = delete;

    bool is_lock_free() const noexcept { return __atomic_is_lock_free(sizeof(T), m_ptr); }

    T load(memory_order order = memory_order_seq_cst) const noexcept
    {
        alignas(T) unsigned char buffer[sizeof(T)];
        __atomic_load(m_ptr, reinterpret_cast<T*>(buffer), toBuiltin(order));
        return *reinterpret_cast<T*>(buffer);
    }
    operator T() const noexcept { return load(); }

    void store(T desired, memory_order order = memory_order_seq_cst) const noexcept
    {
        __atomic_store(m_ptr, __builtin_addressof(desired), toBuiltin(order));
    }
    T operator=(T desired) const noexcept { store(desired); return desired; }

    T exchange(T desired, memory_order order = memory_order_seq_cst) const noexcept
    {
        alignas(T) unsigned char buffer[sizeof(T)];
        __atomic_exchange(m_ptr, __builtin_addressof(desired), reinterpret_cast<T*>(buffer), toBuiltin(order));
        return *reinterpret_cast<T*>(buffer);
    }

    bool compare_exchange_weak(T& expected, T desired, memory_order success, memory_order failure) const noexcept
    {
        return __atomic_compare_exchange(m_ptr, __builtin_addressof(expected), __builtin_addressof(desired), true, toBuiltin(success), toBuiltin(failure));
    }
    bool compare_exchange_weak(T& expected, T desired, memory_order order = memory_order_seq_cst) const noexcept
    {
        return compare_exchange_weak(expected, desired, order, failureOrder(order));
    }
    bool compare_exchange_strong(T& expected, T desired, memory_order success, memory_order failure) const noexcept
    {
        return __atomic_compare_exchange(m_ptr, __builtin_addressof(expected), __builtin_addressof(desired), false, toBuiltin(success), toBuiltin(failure));
    }
    bool compare_exchange_strong(T& expected, T desired, memory_order order = memory_order_seq_cst) const noexcept
    {
        return compare_exchange_strong(expected, desired, order, failureOrder(order));
    }

private:
    template<typename Op>
    T casLoop(Op op, memory_order order) const noexcept
    {
        T expected = load(memory_order_relaxed);
        while (!compare_exchange_weak(expected, op(expected), order, memory_order_relaxed)) { }
        return expected;
    }

public:
    template<typename U = T> requires (is_integral_v<U> || is_pointer_v<U> || is_floating_point_v<U>)
    T fetch_add(difference_type operand, memory_order order = memory_order_seq_cst) const noexcept
    {
        if constexpr (is_integral_v<T>)
            return __atomic_fetch_add(m_ptr, operand, toBuiltin(order));
        else if constexpr (is_pointer_v<T>)
            return __atomic_fetch_add(m_ptr, operand * static_cast<ptrdiff_t>(sizeof(remove_pointer_t<T>)), toBuiltin(order));
        else
            return casLoop([&](T v) { return v + operand; }, order);
    }
    template<typename U = T> requires (is_integral_v<U> || is_pointer_v<U> || is_floating_point_v<U>)
    T fetch_sub(difference_type operand, memory_order order = memory_order_seq_cst) const noexcept
    {
        if constexpr (is_integral_v<T>)
            return __atomic_fetch_sub(m_ptr, operand, toBuiltin(order));
        else if constexpr (is_pointer_v<T>)
            return __atomic_fetch_sub(m_ptr, operand * static_cast<ptrdiff_t>(sizeof(remove_pointer_t<T>)), toBuiltin(order));
        else
            return casLoop([&](T v) { return v - operand; }, order);
    }
    template<typename U = T> requires is_integral_v<U>
    T fetch_and(T operand, memory_order order = memory_order_seq_cst) const noexcept { return __atomic_fetch_and(m_ptr, operand, toBuiltin(order)); }
    template<typename U = T> requires is_integral_v<U>
    T fetch_or(T operand, memory_order order = memory_order_seq_cst) const noexcept { return __atomic_fetch_or(m_ptr, operand, toBuiltin(order)); }
    template<typename U = T> requires is_integral_v<U>
    T fetch_xor(T operand, memory_order order = memory_order_seq_cst) const noexcept { return __atomic_fetch_xor(m_ptr, operand, toBuiltin(order)); }
};

_LIBCPP_END_NAMESPACE_STD

#endif
