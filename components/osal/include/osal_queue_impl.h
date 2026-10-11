// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// Queue<T> template implementation — included by osal.h, do not include directly.

#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace osal {

template <typename T>
Queue<T>::Queue(uint32_t capacity) {
    handle_ = static_cast<void*>(xQueueCreate(capacity, sizeof(T)));
    assert(handle_ != nullptr);
}

template <typename T>
Queue<T>::~Queue() {
    if (handle_) {
        vQueueDelete(static_cast<QueueHandle_t>(handle_));
    }
}

template <typename T>
bool Queue<T>::Send(const T& item, uint32_t timeout_ms) {
    auto qh = static_cast<QueueHandle_t>(handle_);
    if (!xPortInIsrContext()) {
        return xQueueSend(qh, &item, MsToTicks(timeout_ms)) == pdTRUE;
    }
    BaseType_t higher_priority_woken = pdFALSE;
    auto ret = xQueueSendFromISR(qh, &item, &higher_priority_woken);
    portYIELD_FROM_ISR(higher_priority_woken);
    return ret == pdTRUE;
}

template <typename T>
bool Queue<T>::Receive(T& item, uint32_t timeout_ms) {
    return xQueueReceive(static_cast<QueueHandle_t>(handle_),
                         &item, MsToTicks(timeout_ms)) == pdTRUE;
}

template <typename T>
std::optional<T> Queue<T>::Receive(uint32_t timeout_ms) {
    T item;
    if (Receive(item, timeout_ms)) {
        return item;
    }
    return std::nullopt;
}

template <typename T>
uint32_t Queue<T>::Count() const {
    return uxQueueMessagesWaiting(static_cast<QueueHandle_t>(handle_));
}

}  // namespace osal
