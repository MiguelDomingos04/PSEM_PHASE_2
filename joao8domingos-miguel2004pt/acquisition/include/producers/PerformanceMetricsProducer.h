#pragma once
#include "Producer.h"
#include "freertos/queue.h"
#include <cstdint>

// IDs das métricas
#define PRODUCER_ID_CPU_USAGE   0x06
#define PRODUCER_ID_QUEUE_SIZE  0x07
#define PRODUCER_ID_TICK_HEALTH 0x08

class PerformanceMetricsProducer : public Producer {
private:
    
    uint32_t      sample_ms;        // frequência de amostragem 

    // Task Tick Health — mede se há deadlines a ser falhadas
    TickType_t last_wake_tick = 0;
    bool       first_run      = true; // para ignorar a primeira medição, onde o delta será grande. A partir da segunda medição, o delta deve ser aproximadamente igual a sample_ms convertido para ticks. Se for muito maior, indica que a task foi atrasada ou preemptada por muito tempo, o que pode indicar sobrecarga do sistema.

    void sendMetric(uint8_t metric_id, float value); // função auxiliar para enviar um topic_t formatado para a queue
    float estimateCpuUsage(); // estima a utilização da CPU 
    float getTickHealth(); // calcula a saúde dos ticks com base no delta entre ticks e sample_ms
    float getQueueSize();

public:
    // monitoredQueue — queue central do TelemetryManager
    // sample_ms — frequência de amostragem (ex: 5000ms = 5 segundos)
    PerformanceMetricsProducer(uint32_t sample_ms = 5000);

    void setup() override; 
    void run()   override;
};