// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// Operating System Abstraction Layer (OSAL)
// Modern C++ wrapper for FreeRTOS primitives on ESP32.

#pragma once

#include <cassert>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>

namespace osal {

// ---------------------------------------------------------------------------
// Byte buffer aliases
// ---------------------------------------------------------------------------
using InputBuffer = std::basic_string_view<uint8_t>;
using OutputBuffer = std::basic_string<uint8_t>;

// ---------------------------------------------------------------------------
// Free functions -- time, delay, scheduling
// ---------------------------------------------------------------------------

/// Convert milliseconds to FreeRTOS ticks.
uint32_t MsToTicks(uint32_t ms);

/// Block current task for |ms| milliseconds.
void Delay(uint32_t ms);

/// Yield the current task.
void Yield();

/// Monotonic time since boot.
uint64_t TimeUs();
uint64_t TimeMs();
uint32_t TimeSec();

/// FreeRTOS tick helpers.
uint32_t TickRate();
uint32_t TickCount();
uint32_t TickMs();

/// True if the caller is in ISR context (ESP32).
bool InIsrContext();

/// Suspend / resume the FreeRTOS scheduler.
void SuspendScheduler();
void ResumeScheduler();

/// Heap information.
size_t FreeHeapSize();
size_t MinFreeHeapSize();
size_t FreePsramSize();

// ---------------------------------------------------------------------------
// Task
// ---------------------------------------------------------------------------

enum class CoreAffinity : int8_t {
    kCore0 = 0,
    kCore1 = 1,
    kAuto  = -1,
};

enum class TaskState : uint8_t {
    kRunning = 0,
    kReady,
    kBlocked,
    kSuspended,
    kDeleted,
    kInvalid,
};

class Task {
 public:
    using Function = void (*)(void*);

    struct Config {
        const char* name       = "task";
        uint32_t    stack_size = 4096;
        uint32_t    priority   = 5;
        bool        use_psram  = false;
        CoreAffinity core      = CoreAffinity::kAuto;
    };

    Task() = default;
    ~Task() = default;

    // Non-copyable, movable.
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    Task(Task&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    Task& operator=(Task&& other) noexcept;

    /// Create and start a task.
    void Create(Function func, void* arg, const Config& config);

    /// Delete the task. Handle becomes invalid after this call.
    void Delete();

    void Suspend();
    void Resume();

    void SetPriority(uint32_t priority);
    [[nodiscard]] uint32_t GetPriority() const;
    [[nodiscard]] uint32_t GetMinStackRemaining() const;
    [[nodiscard]] TaskState GetState() const;
    [[nodiscard]] std::string_view GetName() const;

    [[nodiscard]] bool IsValid() const { return handle_ != nullptr; }
    [[nodiscard]] void* RawHandle() const { return handle_; }

    /// Returns true if the calling task is this task.
    /// Useful for re-entrancy guards in active-object patterns.
    [[nodiscard]] bool IsCurrent() const;

 private:
    void* handle_ = nullptr;
};

/// Get the name of the calling task (or |handle| if provided).
std::string_view CurrentTaskName(void* handle = nullptr);

// ---------------------------------------------------------------------------
// Mutex
// ---------------------------------------------------------------------------

class Mutex {
 public:
    Mutex();
    ~Mutex();

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    /// Returns true on success, false on timeout.
    [[nodiscard]] bool Lock(uint32_t timeout_ms = UINT32_MAX);
    void Unlock();

 private:
    void* handle_;
};

// ---------------------------------------------------------------------------
// RecursiveMutex
// ---------------------------------------------------------------------------

class RecursiveMutex {
 public:
    RecursiveMutex();
    ~RecursiveMutex();

    RecursiveMutex(const RecursiveMutex&) = delete;
    RecursiveMutex& operator=(const RecursiveMutex&) = delete;

    [[nodiscard]] bool Lock(uint32_t timeout_ms = UINT32_MAX);
    void Unlock();

 private:
    void* handle_;
};

// ---------------------------------------------------------------------------
// LockGuard -- RAII lock for Mutex / RecursiveMutex
// ---------------------------------------------------------------------------

template <typename MutexType>
class LockGuard {
 public:
    explicit LockGuard(MutexType& mutex) : mutex_(mutex) {
        [[maybe_unused]] bool ok = mutex_.Lock();
        assert(ok && "LockGuard: mutex lock timed out");
    }
    ~LockGuard() { mutex_.Unlock(); }

    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

 private:
    MutexType& mutex_;
};

// ---------------------------------------------------------------------------
// Semaphore (binary)
// ---------------------------------------------------------------------------

class Semaphore {
 public:
    Semaphore();
    ~Semaphore();

    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;

    /// Returns true on success, false on timeout.
    [[nodiscard]] bool Take(uint32_t timeout_ms = UINT32_MAX);
    void Give();

 private:
    void* handle_;
};

// ---------------------------------------------------------------------------
// CountingSemaphore -- used by the display's band double-buffering
// ---------------------------------------------------------------------------

class CountingSemaphore {
 public:
    /// |max_count| is the ceiling, |initial_count| the starting count.
    CountingSemaphore(uint32_t max_count, uint32_t initial_count);
    ~CountingSemaphore();

    CountingSemaphore(const CountingSemaphore&) = delete;
    CountingSemaphore& operator=(const CountingSemaphore&) = delete;

    [[nodiscard]] bool Take(uint32_t timeout_ms = UINT32_MAX);
    void Give();
    [[nodiscard]] uint32_t Count() const;

 private:
    void* handle_;
};

// ---------------------------------------------------------------------------
// Queue -- type-safe FreeRTOS queue
// ---------------------------------------------------------------------------

template <typename T>
class Queue {
 public:
    explicit Queue(uint32_t capacity);
    ~Queue();

    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;

    /// Send an item. ISR-safe (timeout ignored in ISR).  Returns true on success.
    bool Send(const T& item, uint32_t timeout_ms = UINT32_MAX);

    /// Receive an item. Returns true on success. NOT ISR-safe.
    bool Receive(T& item, uint32_t timeout_ms = UINT32_MAX);

    /// Convenience: receive with std::optional.
    std::optional<T> Receive(uint32_t timeout_ms = UINT32_MAX);

    /// Number of items currently in the queue.
    [[nodiscard]] uint32_t Count() const;

 private:
    void* handle_;
};

// ---------------------------------------------------------------------------
// EventGroup
// ---------------------------------------------------------------------------

class EventGroup {
 public:
    EventGroup();
    ~EventGroup();

    EventGroup(const EventGroup&) = delete;
    EventGroup& operator=(const EventGroup&) = delete;

    void SetBits(uint32_t bits);
    void ClearBits(uint32_t bits);
    [[nodiscard]] uint32_t GetBits() const;

    /// Wait for ANY of |bits|. Returns received bits, or std::nullopt on timeout.
    std::optional<uint32_t> WaitAny(uint32_t bits, bool auto_clear = true,
                                    uint32_t timeout_ms = UINT32_MAX);

    /// Wait for ALL of |bits|. Returns received bits, or std::nullopt on timeout.
    std::optional<uint32_t> WaitAll(uint32_t bits, bool auto_clear = true,
                                    uint32_t timeout_ms = UINT32_MAX);

 private:
    void* handle_;
};

// ---------------------------------------------------------------------------
// RingBuffer (no-split mode)
// ---------------------------------------------------------------------------

class RingBuffer {
 public:
    explicit RingBuffer(uint32_t size);
    ~RingBuffer();

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    /// Send data into the ring buffer. Returns true on success.
    bool Send(InputBuffer buf, uint32_t timeout_ms = UINT32_MAX);

    /// Receive data. The returned view points into the ring buffer's internal
    /// storage -- call ReturnItem() when done.  Empty view on timeout.
    InputBuffer Receive(uint32_t timeout_ms = UINT32_MAX);

    /// Return a previously received item back to the ring buffer.
    void ReturnItem(InputBuffer buf);

    [[nodiscard]] uint32_t FreeSize() const;

 private:
    void* handle_;
};

// ---------------------------------------------------------------------------
// Timer (FreeRTOS software timer)
// ---------------------------------------------------------------------------

class Timer {
 public:
    using Callback = std::function<void(Timer&)>;

    Timer(Callback callback, uint32_t period_ms, bool auto_reload = true,
          const char* name = "timer");
    ~Timer();

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    void Start();
    void Stop();
    void Restart();
    void Restart(uint32_t new_period_ms);

    [[nodiscard]] bool IsRunning() const;
    [[nodiscard]] uint32_t GetPeriod() const;
    [[nodiscard]] const char* GetName() const;

    // Called by the FreeRTOS timer callback adapter; not intended for direct use.
    void FireCallback() { if (callback_) callback_(*this); }

 private:
    void* handle_;
    Callback callback_;
};

// ---------------------------------------------------------------------------
// CriticalSection -- RAII wrapper for taskENTER/EXIT_CRITICAL
// ---------------------------------------------------------------------------

class CriticalSection {
 public:
    CriticalSection();
    ~CriticalSection();

    CriticalSection(const CriticalSection&) = delete;
    CriticalSection& operator=(const CriticalSection&) = delete;
};

// ---------------------------------------------------------------------------
// DeltaTimer -- measure elapsed time between calls
// ---------------------------------------------------------------------------

class DeltaTimer {
 public:
    DeltaTimer() : last_us_(TimeUs()) {}

    uint32_t ElapsedUs();
    uint32_t ElapsedMs() { return ElapsedUs() / 1000; }
    uint32_t ElapsedSec() { return ElapsedUs() / 1000000; }

    void Reset() { last_us_ = TimeUs(); }

 private:
    uint64_t last_us_;
};

// ---------------------------------------------------------------------------
// Queue template implementation (must be in header)
// ---------------------------------------------------------------------------

}  // namespace osal

// Include the template implementation after the namespace closing so we can
// use FreeRTOS headers only in the implementation header.
#include "osal_queue_impl.h"
