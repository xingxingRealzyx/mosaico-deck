// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// Operating System Abstraction Layer — implementation.

#include "osal.h"

#include "esp_timer.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/ringbuf.h"

#include "esp_log.h"
#include "esp_heap_caps.h"

#if CONFIG_SPIRAM
#include <esp_private/freertos_idf_additions_priv.h>
#endif

static const char* kTag = "osal";

namespace osal {

// ===========================================================================
// Free functions
// ===========================================================================

uint32_t MsToTicks(uint32_t ms) {
    if (ms == UINT32_MAX) {
        return portMAX_DELAY;
    }
    return pdMS_TO_TICKS(ms);
}

void Delay(uint32_t ms) { vTaskDelay(MsToTicks(ms)); }
void Yield() { taskYIELD(); }

uint64_t TimeUs() {
    // 必须用 esp_timer 的**单调**时钟，不能用 gettimeofday：
    // 后者会被 SNTP 校时 / settimeofday 往前后拉，而仿真步长是靠两次
    // TimeUs() 相减算出来的 —— 时间被往回拨一次，就会算出一个负的 dt。
    // input.cpp 一直是直接用 esp_timer_get_time()，这里统一过来。
    return static_cast<uint64_t>(esp_timer_get_time());
}

uint64_t TimeMs() { return TimeUs() / 1000ULL; }

uint32_t TimeSec() {
    timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<uint32_t>(tv.tv_sec);
}

uint32_t TickRate() { return configTICK_RATE_HZ; }
uint32_t TickCount() { return xTaskGetTickCount(); }
uint32_t TickMs() { return TickCount() * portTICK_PERIOD_MS; }

bool InIsrContext() { return xPortInIsrContext(); }

void SuspendScheduler() { vTaskSuspendAll(); }
void ResumeScheduler() { xTaskResumeAll(); }

size_t FreeHeapSize() { return esp_get_free_heap_size(); }
size_t MinFreeHeapSize() { return esp_get_minimum_free_heap_size(); }
size_t FreePsramSize() { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }

// ===========================================================================
// Task
// ===========================================================================

Task& Task::operator=(Task&& other) noexcept {
    if (this != &other) {
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

void Task::Create(Function func, void* arg, const Config& config) {
    int core_id = static_cast<int>(config.core);
    BaseType_t affinity = (core_id < 0) ? tskNO_AFFINITY : core_id;

#if CONFIG_SPIRAM
    if (config.use_psram) {
        int caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        prvTaskCreateDynamicPinnedToCoreWithCaps(
            func, config.name, config.stack_size, arg,
            config.priority, affinity, caps,
            reinterpret_cast<TaskHandle_t*>(&handle_));
    } else {
        xTaskCreatePinnedToCore(
            func, config.name, config.stack_size, arg,
            config.priority, reinterpret_cast<TaskHandle_t*>(&handle_),
            affinity);
    }
#else
    xTaskCreatePinnedToCore(
        func, config.name, config.stack_size, arg,
        config.priority, reinterpret_cast<TaskHandle_t*>(&handle_),
        affinity);
#endif

    assert(handle_ != nullptr);
    ESP_LOGI(kTag, "Task \"%s\" created (stack=%lu, pri=%lu, core=%d)",
             config.name, config.stack_size, config.priority, core_id);
}

void Task::Delete() {
    assert(handle_ != nullptr);
    ESP_LOGI(kTag, "Task \"%.*s\" deleted",
             static_cast<int>(GetName().size()), GetName().data());
    vTaskDelete(static_cast<TaskHandle_t>(handle_));
    handle_ = nullptr;
}

void Task::Suspend() { vTaskSuspend(static_cast<TaskHandle_t>(handle_)); }

void Task::Resume() {
    auto th = static_cast<TaskHandle_t>(handle_);
    if (!InIsrContext()) {
        vTaskResume(th);
    } else {
        BaseType_t yield = xTaskResumeFromISR(th);
        portYIELD_FROM_ISR(yield);
    }
}

void Task::SetPriority(uint32_t priority) {
    vTaskPrioritySet(static_cast<TaskHandle_t>(handle_), priority);
}

uint32_t Task::GetPriority() const {
    return uxTaskPriorityGet(static_cast<TaskHandle_t>(handle_));
}

uint32_t Task::GetMinStackRemaining() const {
    return uxTaskGetStackHighWaterMark(static_cast<TaskHandle_t>(handle_));
}

TaskState Task::GetState() const {
    return static_cast<TaskState>(
        eTaskGetState(static_cast<TaskHandle_t>(handle_)));
}

std::string_view Task::GetName() const {
    return pcTaskGetName(static_cast<TaskHandle_t>(handle_));
}

bool Task::IsCurrent() const {
    if (handle_ == nullptr) return false;
    return static_cast<TaskHandle_t>(handle_) == xTaskGetCurrentTaskHandle();
}

std::string_view CurrentTaskName(void* handle) {
    return pcTaskGetName(static_cast<TaskHandle_t>(handle));
}

// ===========================================================================
// Mutex
// ===========================================================================

Mutex::Mutex() : handle_(static_cast<void*>(xSemaphoreCreateMutex())) {
    assert(handle_ != nullptr);
}

Mutex::~Mutex() {
    vSemaphoreDelete(static_cast<SemaphoreHandle_t>(handle_));
}

bool Mutex::Lock(uint32_t timeout_ms) {
    return xSemaphoreTake(static_cast<SemaphoreHandle_t>(handle_),
                          MsToTicks(timeout_ms)) == pdTRUE;
}

void Mutex::Unlock() {
    auto sh = static_cast<SemaphoreHandle_t>(handle_);
    if (!InIsrContext()) {
        xSemaphoreGive(sh);
    } else {
        BaseType_t higher_priority_woken = pdFALSE;
        xSemaphoreGiveFromISR(sh, &higher_priority_woken);
        portYIELD_FROM_ISR(higher_priority_woken);
    }
}

// ===========================================================================
// RecursiveMutex
// ===========================================================================

RecursiveMutex::RecursiveMutex()
    : handle_(static_cast<void*>(xSemaphoreCreateRecursiveMutex())) {
    assert(handle_ != nullptr);
}

RecursiveMutex::~RecursiveMutex() {
    vSemaphoreDelete(static_cast<SemaphoreHandle_t>(handle_));
}

bool RecursiveMutex::Lock(uint32_t timeout_ms) {
    return xSemaphoreTakeRecursive(static_cast<SemaphoreHandle_t>(handle_),
                                   MsToTicks(timeout_ms)) == pdTRUE;
}

void RecursiveMutex::Unlock() {
    xSemaphoreGiveRecursive(static_cast<SemaphoreHandle_t>(handle_));
}

// ===========================================================================
// Semaphore
// ===========================================================================

Semaphore::Semaphore()
    : handle_(static_cast<void*>(xSemaphoreCreateBinary())) {
    assert(handle_ != nullptr);
}

Semaphore::~Semaphore() {
    vSemaphoreDelete(static_cast<SemaphoreHandle_t>(handle_));
}

bool Semaphore::Take(uint32_t timeout_ms) {
    return xSemaphoreTake(static_cast<SemaphoreHandle_t>(handle_),
                          MsToTicks(timeout_ms)) == pdTRUE;
}

void Semaphore::Give() {
    auto sh = static_cast<SemaphoreHandle_t>(handle_);
    if (!InIsrContext()) {
        xSemaphoreGive(sh);
    } else {
        BaseType_t higher_priority_woken = pdFALSE;
        xSemaphoreGiveFromISR(sh, &higher_priority_woken);
        portYIELD_FROM_ISR(higher_priority_woken);
    }
}

// ===========================================================================
// CountingSemaphore
// ===========================================================================

CountingSemaphore::CountingSemaphore(uint32_t max_count, uint32_t initial_count)
    : handle_(static_cast<void*>(
          xSemaphoreCreateCounting(max_count, initial_count))) {
    assert(handle_ != nullptr);
}

CountingSemaphore::~CountingSemaphore() {
    vSemaphoreDelete(static_cast<SemaphoreHandle_t>(handle_));
}

bool CountingSemaphore::Take(uint32_t timeout_ms) {
    return xSemaphoreTake(static_cast<SemaphoreHandle_t>(handle_),
                          MsToTicks(timeout_ms)) == pdTRUE;
}

void CountingSemaphore::Give() {
    auto sh = static_cast<SemaphoreHandle_t>(handle_);
    if (!InIsrContext()) {
        xSemaphoreGive(sh);
    } else {
        BaseType_t higher_priority_woken = pdFALSE;
        xSemaphoreGiveFromISR(sh, &higher_priority_woken);
        portYIELD_FROM_ISR(higher_priority_woken);
    }
}

uint32_t CountingSemaphore::Count() const {
    return static_cast<uint32_t>(
        uxSemaphoreGetCount(static_cast<SemaphoreHandle_t>(handle_)));
}

// ===========================================================================
// EventGroup
// ===========================================================================

EventGroup::EventGroup()
    : handle_(static_cast<void*>(xEventGroupCreate())) {
    assert(handle_ != nullptr);
}

EventGroup::~EventGroup() {
    vEventGroupDelete(static_cast<EventGroupHandle_t>(handle_));
}

void EventGroup::SetBits(uint32_t bits) {
    auto eh = static_cast<EventGroupHandle_t>(handle_);
    if (!InIsrContext()) {
        xEventGroupSetBits(eh, bits);
    } else {
        BaseType_t higher_priority_woken = pdFALSE;
        xEventGroupSetBitsFromISR(eh, bits, &higher_priority_woken);
        portYIELD_FROM_ISR(higher_priority_woken);
    }
}

void EventGroup::ClearBits(uint32_t bits) {
    xEventGroupClearBits(static_cast<EventGroupHandle_t>(handle_), bits);
}

uint32_t EventGroup::GetBits() const {
    return xEventGroupGetBits(static_cast<EventGroupHandle_t>(handle_));
}

std::optional<uint32_t> EventGroup::WaitAny(uint32_t bits, bool auto_clear,
                                            uint32_t timeout_ms) {
    uint32_t received = xEventGroupWaitBits(
        static_cast<EventGroupHandle_t>(handle_),
        bits, auto_clear ? pdTRUE : pdFALSE, pdFALSE, MsToTicks(timeout_ms));
    if (received & bits) {
        return received;
    }
    return std::nullopt;
}

std::optional<uint32_t> EventGroup::WaitAll(uint32_t bits, bool auto_clear,
                                            uint32_t timeout_ms) {
    uint32_t received = xEventGroupWaitBits(
        static_cast<EventGroupHandle_t>(handle_),
        bits, auto_clear ? pdTRUE : pdFALSE, pdTRUE, MsToTicks(timeout_ms));
    if ((received & bits) == bits) {
        return received;
    }
    return std::nullopt;
}

// ===========================================================================
// RingBuffer
// ===========================================================================

RingBuffer::RingBuffer(uint32_t size)
    : handle_(xRingbufferCreate(size, RINGBUF_TYPE_NOSPLIT)) {
    assert(handle_ != nullptr);
}

RingBuffer::~RingBuffer() {
    if (handle_) {
        vRingbufferDelete(handle_);
    }
}

bool RingBuffer::Send(InputBuffer buf, uint32_t timeout_ms) {
    return xRingbufferSend(handle_, buf.data(), buf.size(),
                           MsToTicks(timeout_ms)) == pdTRUE;
}

InputBuffer RingBuffer::Receive(uint32_t timeout_ms) {
    size_t len = 0;
    auto* ptr = static_cast<uint8_t*>(
        xRingbufferReceive(handle_, &len, MsToTicks(timeout_ms)));
    return InputBuffer{ptr, len};
}

void RingBuffer::ReturnItem(InputBuffer buf) {
    vRingbufferReturnItem(handle_, const_cast<uint8_t*>(buf.data()));
}

uint32_t RingBuffer::FreeSize() const {
    return xRingbufferGetCurFreeSize(handle_);
}

// ===========================================================================
// Timer
// ===========================================================================

// File-local adapter: bridges FreeRTOS C callback into Timer::FireCallback().
static void TimerCallbackAdapter(TimerHandle_t timer_handle) {
    auto* self = static_cast<Timer*>(pvTimerGetTimerID(timer_handle));
    if (self) self->FireCallback();
}

Timer::Timer(Callback callback, uint32_t period_ms, bool auto_reload,
             const char* name)
    : callback_(std::move(callback)) {
    handle_ = static_cast<void*>(xTimerCreate(
        name, MsToTicks(period_ms), auto_reload ? pdTRUE : pdFALSE,
        this,  // pvTimerID points back to this Timer instance
        TimerCallbackAdapter));
    assert(handle_ != nullptr);
    ESP_LOGI(kTag, "Timer \"%s\" created (period=%lums)", name,
             static_cast<unsigned long>(period_ms));
}

Timer::~Timer() {
    xTimerDelete(static_cast<TimerHandle_t>(handle_), portMAX_DELAY);
}

void Timer::Start() {
    auto th = static_cast<TimerHandle_t>(handle_);
    if (!InIsrContext()) {
        xTimerStart(th, portMAX_DELAY);
    } else {
        BaseType_t higher_priority_woken = pdFALSE;
        xTimerStartFromISR(th, &higher_priority_woken);
        portYIELD_FROM_ISR(higher_priority_woken);
    }
}

void Timer::Stop() {
    auto th = static_cast<TimerHandle_t>(handle_);
    if (!InIsrContext()) {
        xTimerStop(th, portMAX_DELAY);
    } else {
        BaseType_t higher_priority_woken = pdFALSE;
        xTimerStopFromISR(th, &higher_priority_woken);
        portYIELD_FROM_ISR(higher_priority_woken);
    }
}

void Timer::Restart() {
    auto th = static_cast<TimerHandle_t>(handle_);
    if (!InIsrContext()) {
        xTimerReset(th, portMAX_DELAY);
    } else {
        BaseType_t higher_priority_woken = pdFALSE;
        xTimerResetFromISR(th, &higher_priority_woken);
        portYIELD_FROM_ISR(higher_priority_woken);
    }
}

void Timer::Restart(uint32_t new_period_ms) {
    if (new_period_ms == 0) return;
    auto th = static_cast<TimerHandle_t>(handle_);
    if (!InIsrContext()) {
        xTimerChangePeriod(th, MsToTicks(new_period_ms), portMAX_DELAY);
    } else {
        BaseType_t higher_priority_woken = pdFALSE;
        xTimerChangePeriodFromISR(th, MsToTicks(new_period_ms),
                                  &higher_priority_woken);
        portYIELD_FROM_ISR(higher_priority_woken);
    }
}

bool Timer::IsRunning() const {
    return xTimerIsTimerActive(static_cast<TimerHandle_t>(handle_)) != pdFALSE;
}

uint32_t Timer::GetPeriod() const {
    return pdTICKS_TO_MS(
        xTimerGetPeriod(static_cast<TimerHandle_t>(handle_)));
}

const char* Timer::GetName() const {
    return pcTimerGetName(static_cast<TimerHandle_t>(handle_));
}

// ===========================================================================
// CriticalSection
// ===========================================================================

static spinlock_t g_spinlock = SPINLOCK_INITIALIZER;

CriticalSection::CriticalSection() { taskENTER_CRITICAL(&g_spinlock); }
CriticalSection::~CriticalSection() { taskEXIT_CRITICAL(&g_spinlock); }

// ===========================================================================
// DeltaTimer
// ===========================================================================

uint32_t DeltaTimer::ElapsedUs() {
    uint64_t now = TimeUs();
    uint32_t dt = static_cast<uint32_t>(now - last_us_);
    last_us_ = now;
    return dt;
}

}  // namespace osal

// ===========================================================================
// FreeRTOS hook functions (weak — user can override)
// ===========================================================================

extern "C" __attribute__((weak))
void vApplicationMallocFailedHook() {
    ESP_LOGE("osal", "Malloc failed!");
    assert(0);
}

extern "C" __attribute__((weak))
void vApplicationStackOverflowHook(TaskHandle_t task, char* name) {
    ESP_LOGE("osal", "Stack overflow in task: %s", name);
    assert(0);
}
