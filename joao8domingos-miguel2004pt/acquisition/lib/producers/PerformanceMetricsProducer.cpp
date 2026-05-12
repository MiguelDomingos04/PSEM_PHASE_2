#include "PerformanceMetricsProducer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

#define TAG "PerformanceMetrics"

// Força a visibilidade das funções de Stats do FreeRTOS
extern "C" {
    uint32_t ulTaskGetIdleRunTimeCounterHandle(TaskHandle_t xTask);
    TaskHandle_t xTaskGetIdleTaskHandleForCore(BaseType_t xCoreID);
}

PerformanceMetricsProducer::PerformanceMetricsProducer(uint32_t sample_ms) 
    : Producer(PRODUCER_ID_CPU_CORE_0), sample_ms(sample_ms)
{}

void PerformanceMetricsProducer::setup()
{
    ESP_LOGI(TAG, "PerformanceMetricsProducer inicializado — Monitorização Dual Core ativa");
}

void PerformanceMetricsProducer::run()
{
    // Inicialização do tick no primeiro ciclo para evitar saltos na métrica de saúde
    if (first_run) {
        last_wake_tick = xTaskGetTickCount();
        first_run      = false;
    }

    vTaskDelay(pdMS_TO_TICKS(sample_ms));

    // 1. Envio das métricas de CPU para ambos os núcleos
    sendMetric(PRODUCER_ID_CPU_CORE_0, estimateCpuUsage(0));
    sendMetric(PRODUCER_ID_CPU_CORE_1, estimateCpuUsage(1));

    // 2. Tamanho da Queue central
    sendMetric(PRODUCER_ID_QUEUE_SIZE, getQueueSize());

    // 3. Saúde dos Ticks (Atrasos de agendamento)
    sendMetric(PRODUCER_ID_TICK_HEALTH, getTickHealth());
}

void PerformanceMetricsProducer::sendMetric(uint8_t metric_id, float value)
{
    topic_t topic = {
        .producer_id  = metric_id,
        .timestamp_us = static_cast<uint64_t>(esp_timer_get_time()),
        .value        = value,
    };

    if (xQueueSend(destinationQueue, &topic, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Queue cheia — métrica 0x%02X descartada", metric_id);
    }
}

float PerformanceMetricsProducer::estimateCpuUsage(int core_id)
{
    // Obtém o identificador da tarefa "Idle" específica deste núcleo
    TaskHandle_t idle_task_handle = xTaskGetIdleTaskHandleForCore(core_id);
    
    if (idle_task_handle == nullptr) return 0.0f;

    // Tempo total que o CPU passou em repouso (idle)
    uint32_t idle_ticks  = ulTaskGetIdleRunTimeCounterHandle(idle_task_handle);
    // Tempo total do sistema (convertido para milissegundos/unidade de tick)
    uint32_t total_ticks = (uint32_t)(esp_timer_get_time() / 1000);

    // Cálculo da variação (delta) entre a leitura atual e a anterior
    uint32_t delta_idle  = idle_ticks  - last_idle_ticks[core_id];
    uint32_t delta_total = total_ticks - last_total_ticks[core_id];

    // Atualiza o histórico para o próximo ciclo
    last_idle_ticks[core_id]  = idle_ticks;
    last_total_ticks[core_id] = total_ticks;

    if (delta_total == 0) return 0.0f;

    // O uso do CPU é o complemento da percentagem de tempo em idle
    float idle_pct = ((float)delta_idle / (float)delta_total) * 100.0f;
    float cpu_pct  = 100.0f - idle_pct;

    // Garante que o valor está no intervalo [0, 100]
    if (cpu_pct < 0.0f)   cpu_pct = 0.0f;
    if (cpu_pct > 100.0f) cpu_pct = 100.0f;

    return cpu_pct;
}

float PerformanceMetricsProducer::getQueueSize()
{
    if (destinationQueue == nullptr) return -1.0f;
    return (float)uxQueueMessagesWaiting(destinationQueue);
}

float PerformanceMetricsProducer::getTickHealth()
{
    TickType_t now          = xTaskGetTickCount();
    TickType_t elapsed_ms   = (now - last_wake_tick) * portTICK_PERIOD_MS;
    float      deviation_ms = (float)elapsed_ms - (float)sample_ms;

    last_wake_tick = now;

    return deviation_ms > 0.0f ? deviation_ms : 0.0f;
}