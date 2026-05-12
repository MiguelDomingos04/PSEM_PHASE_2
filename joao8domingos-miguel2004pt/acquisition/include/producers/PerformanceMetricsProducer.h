#pragma once
#include "Producer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <cstdint>

// IDs das métricas
#define PRODUCER_ID_CPU_CORE_0  0x06  // Antigo CPU_USAGE, agora mapeado para o Core 0
#define PRODUCER_ID_QUEUE_SIZE  0x07
#define PRODUCER_ID_TICK_HEALTH 0x08
#define PRODUCER_ID_CPU_CORE_1  0x09  // Novo ID para monitorizar o Core 1

class PerformanceMetricsProducer : public Producer {
private:
    uint32_t sample_ms;
    uint16_t   device_id = 0;   // hash XOR do Base MAC, calculado no setup()

    // Armazenamento dos estados de CPU para ambos os núcleos
    uint32_t last_idle_ticks[2]  = {0, 0};
    uint32_t last_total_ticks[2] = {0, 0};

    // Variáveis para cálculo de Tick Health
    TickType_t last_wake_tick = 0;
    bool       first_run      = true;

    // Métodos internos de processamento
    uint16_t computeDeviceId();                     // XOR fold dos 6 bytes do MAC
    void  sendMetric(uint8_t metric_id, float value);
    float estimateCpuUsage(int core_id); 
    float getTickHealth();
    float getQueueSize();

public:
    // Construtor com frequência de amostragem (padrão 5 segundos)
    PerformanceMetricsProducer(uint32_t sample_ms = 5000);

    void setup() override;
    void run()   override;
};

