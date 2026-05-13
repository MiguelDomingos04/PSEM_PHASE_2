#pragma once
#include <cstddef>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "Producer.h"
#include "Consumer.h"
#include "topic.h"

template <size_t MaxProducers, size_t MaxConsumers, size_t QueueSize>
class TelemetryManager {
private:
    Producer*     producers[MaxProducers] = {nullptr};
    size_t        registeredProducers     = 0;

    Consumer*     consumers[MaxConsumers] = {nullptr};
    size_t        registeredConsumers     = 0;

    QueueHandle_t managerQueue = nullptr;
    TaskHandle_t  taskHandle   = nullptr;

    void addProducer(Producer* producer) {
        if (registeredProducers < MaxProducers && producer != nullptr) {
            producer->setQueue(managerQueue);
            producers[registeredProducers++] = producer;
        }
    }

    void addConsumer(Consumer* consumer) {
        if (registeredConsumers < MaxConsumers && consumer != nullptr) {
            consumers[registeredConsumers++] = consumer;
        }
    }

    // Loop principal — lê da queue e distribui pelos consumers
    static void taskLoopWrapper(void* arg) {
        auto* self = static_cast<TelemetryManager*>(arg);
        while (true) {
           topic_t1 topic;
            if (xQueueReceive(self->managerQueue, &topic, portMAX_DELAY) == pdTRUE) {
                for (size_t i = 0; i < self->registeredConsumers; i++) {
                    if (self->consumers[i] != nullptr) {
                        self->consumers[i]->consume(topic);
                    }
                }
            }
        }
    }

    // Wrapper para cada producer task
    static void producerTaskWrapper(void* arg) {
        auto* producer = static_cast<Producer*>(arg);
        producer->setup();
        while (true) {
            producer->run();
        }
    }

    // Wrapper para cada consumer task — chama run() continuamente
    static void consumerTaskWrapper(void* arg) {
        auto* consumer = static_cast<Consumer*>(arg);
        while (true) {
            consumer->run();
        }
    }

public:
    TelemetryManager() {
        managerQueue = xQueueCreate(QueueSize, sizeof(topic_t1));
    }

    ~TelemetryManager() {
        if (managerQueue != nullptr) {
            vQueueDelete(managerQueue);
        }
    }

    template <typename... Producers>
    void registerProducers(Producers*... producersArgs) {
        (addProducer(producersArgs), ...);
    }

    template <typename... Consumers>
    void registerConsumers(Consumers*... consumersArgs) {
        (addConsumer(consumersArgs), ...);
    }

    void run() {
        // 1. Setup de todos os consumers — antes de criar qualquer task
        for (size_t i = 0; i < registeredConsumers; i++) {
            if (consumers[i] != nullptr) {
                consumers[i]->setup();
            }
        }

        // 2. Criar task de run() para cada consumer
        for (size_t i = 0; i < registeredConsumers; i++) {
            if (consumers[i] != nullptr) {
                xTaskCreate(
                    consumerTaskWrapper,
                    "consumer_task",
                    4096,           // stack size de 4KB para cada consumer
                    consumers[i],
                    5,              // prioridade 5 para consumers, 6 para o loop principal, 5 para producers
                    nullptr
                );
            }
        }

        // 3. Criar task para cada producer
        for (size_t i = 0; i < registeredProducers; i++) {
            if (producers[i] != nullptr) {
                xTaskCreate(
                    producerTaskWrapper,
                    "producer_task",
                    4096,
                    producers[i],
                    5,
                    nullptr
                );
            }
        }

        // 4. Criar task do loop principal
        xTaskCreate(
            taskLoopWrapper,
            "manager_task",
            8192,
            this,
            6,
            &taskHandle
        );
    }
};