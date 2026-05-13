#include "producers/VoltageProducer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"
#include <cstdint>

#define TAG          "VoltageProducer"
#define SAMPLE_MS    100   
#define MIN_VOLTAGE  18.0f
#define MAX_VOLTAGE  25.2f
#define MAX_DELTA    0.1f
#define ADC_CHANNEL  ADC_CHANNEL_0   // GPIO1
#define SAMPLE_FREQ  20000
#define FRAME_SIZE   256             // número de bytes por frame do DMA — deve ser múltiplo de SOC_ADC_DIGI_RESULT_BYTES (4 bytes por amostra) e grande o suficiente para conter várias amostras (ex: 256 bytes = 64 amostras)
#define DMA_BUF_SIZE (FRAME_SIZE * 4) // tamanho total do buffer de DMA — deve ser maior que FRAME_SIZE para permitir múltiplos frames

VoltageProducer::VoltageProducer(bool useMockData)
    : Producer(PRODUCER_ID_VOLTAGE), useMockData(useMockData)
{
    current_value = randomFloatInRange(MIN_VOLTAGE, MAX_VOLTAGE); 
}

void VoltageProducer::setup()
{
    if (useMockData) {
        ESP_LOGI(TAG, "VoltageProducer iniciado em modo MOCK — valor inicial: %.2fV",
                 current_value);
    } else {
        setupADC();
        ESP_LOGI(TAG, "VoltageProducer iniciado em modo HARDWARE");
    }
}

void VoltageProducer::run()
{
    if (useMockData) {
        current_value = randomWalk(current_value, MAX_DELTA, MIN_VOLTAGE, MAX_VOLTAGE);

       topic_t1 topic = {
            .producer_id  = producerId,
            .timestamp_us = static_cast<uint64_t>(esp_timer_get_time()),
            .value        = current_value,
        };

        if (xQueueSend(destinationQueue, &topic, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Queue cheia — leitura descartada");
        }

        ESP_LOGI(TAG, "voltage=%.2fV [MOCK]", current_value);
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));

    } else {
        // Guarda o handle da task para a ISR saber quem acordar
        // Feito aqui e não no construtor porque a task só existe quando
        // o TelemetryManager a cria com xTaskCreate
        task_handle = xTaskGetCurrentTaskHandle(); // para a ISR notificar quando há um frame pronto

        // Bloqueia até a ISR notificar que há um frame pronto — igual à Phase 1
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // aguarda notificação da ISR (que indica que o buffer DMA está cheio)

        // Envia umtopic_t1 por amostra do frame
        readADC();
    }
}

// ISR callback 
// Igual ao on_conv_done da Phase 1 — notifica a task quando o buffer está cheio
bool IRAM_ATTR VoltageProducer::onConvDone(adc_continuous_handle_t handle,
                                            const adc_continuous_evt_data_t *edata,
                                            void *user_data)
{
    VoltageProducer *self = static_cast<VoltageProducer*>(user_data); // recupera o ponteiro para a instância da classe
    BaseType_t must_yield = pdFALSE; // variável para indicar se a task deve ser acordada imediatamente
    vTaskNotifyGiveFromISR(self->task_handle, &must_yield); // notifica a task associada ao handle que o buffer DMA está cheio
    return must_yield == pdTRUE; // retorna true se a task deve ser acordada imediatamente (se for de maior prioridade que a task atual)
}

//  setupADC 
void VoltageProducer::setupADC()
{
    adc_continuous_handle_cfg_t handle_cfg = {
        .max_store_buf_size = DMA_BUF_SIZE,     // tamanho total do buffer de DMA  
        .conv_frame_size    = FRAME_SIZE,       // número de bytes por frame do DMA — deve ser múltiplo de SOC_ADC_DIGI_RESULT_BYTES e grande o suficiente para conter várias amostras
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&handle_cfg, &adc_handle));

    adc_digi_pattern_config_t pattern = {
        .atten     = ADC_ATTEN_DB_12,    // atenuação de 12dB para medir até ~3.1V
        .channel   = ADC_CHANNEL,        // canal ADC a ser lido (GPIO1 = ADC_CHANNEL_0)
        .unit      = ADC_UNIT_1,         // usar ADC1 para GPIO1. ADC2 é compartilhado com WiFi e pode ser instável quando o WiFi está ativo
        .bit_width = ADC_BITWIDTH_12,    // resolução de 12 bits (0–4095) para melhor precisão na conversão de tensão
    };

    adc_continuous_config_t dig_cfg = {};
    dig_cfg.sample_freq_hz = SAMPLE_FREQ;               // frequência de amostragem do ADC — deve ser alta o suficiente para capturar as variações de tensão, mas não tão alta a ponto de gerar muitos dados para processar
    dig_cfg.conv_mode      = ADC_CONV_SINGLE_UNIT_1;  // ler apenas do ADC1 (GPIO1) para evitar instabilidade do ADC2 com WiFi ativo
    dig_cfg.format         = ADC_DIGI_OUTPUT_FORMAT_TYPE2;  // formato de saída do ADC — inclui o valor bruto e o canal, facilitando a conversão para tensão
    dig_cfg.adc_pattern    = &pattern;     // padrão de leitura do ADC — define o canal, atenuação e resolução para cada amostra. Neste caso, apenas um canal é lido, mas o framework suporta múltiplos canais com um array de padrões.
    dig_cfg.pattern_num    = 1;         // número de padrões no array — 1 neste caso, mas pode ser maior para múltiplos canais. O framework irá iterar sobre os padrões para

    /*
    adc_continuous_config_t dig_cfg = {
        .sample_freq_hz = SAMPLE_FREQ,               // frequência de amostragem do ADC — deve ser alta o suficiente para capturar as variações de tensão, mas não tão alta a ponto de gerar muitos dados para processar.
        .conv_mode      = ADC_CONV_SINGLE_UNIT_1,    // ler apenas do ADC1 (GPIO1) para evitar instabilidade do ADC2 com WiFi ativo
        .format         = ADC_DIGI_OUTPUT_FORMAT_TYPE2,  // formato de saída do ADC — inclui o valor bruto e o canal, facilitando a conversão para tensão
        .adc_pattern    = &pattern,     // padrão de leitura do ADC — define o canal, atenuação e resolução para cada amostra. Neste caso, apenas um canal é lido, mas o framework suporta múltiplos canais com um array de padrões.
        .pattern_num    = 1,         // número de padrões no array — 1 neste caso, mas pode ser maior para múltiplos canais. O framework irá iterar sobre os padrões para cada amostra, permitindo ler múltiplos canais em sequência.
    };
    */
    ESP_ERROR_CHECK(adc_continuous_config(adc_handle, &dig_cfg));

    // Registar ISR — passa 'this' como user_data para aceder ao task_handle
    adc_continuous_evt_cbs_t cbs = { .on_conv_done = onConvDone };
    ESP_ERROR_CHECK(adc_continuous_register_event_callbacks(adc_handle, &cbs, this));
    ESP_ERROR_CHECK(adc_continuous_start(adc_handle));
}

//  readADC — umtopic_t1 por amostra 
// Igual à Phase 1 — lê frame do DMA e processa cada amostra individualmente
void VoltageProducer::readADC()
{
    uint8_t  buf[FRAME_SIZE];
    uint32_t out_len = 0;

    // Lê um frame do DMA — bloqueia até o buffer estar cheio, o que é sinalizado pela ISR
    esp_err_t ret = adc_continuous_read(adc_handle, buf, FRAME_SIZE, &out_len, 0); // 
    if (ret != ESP_OK) return;

    uint32_t n = out_len / SOC_ADC_DIGI_RESULT_BYTES; // número de amostras no frame (cada amostra tem SOC_ADC_DIGI_RESULT_BYTES bytes)

    for (uint32_t i = 0; i < n; i++) {
        adc_digi_output_data_t *p =
            (adc_digi_output_data_t *)&buf[i * SOC_ADC_DIGI_RESULT_BYTES]; // ponteiro para a amostra atual no buffer

        // Converter raw para tensão — sem calibração formal
        // Na sessão presencial ajustar com o divisor de tensão real
        float voltage = (p->type2.data / 4095.0f) * 3.1f;

       topic_t1 topic = {
            .producer_id  = producerId,
            .timestamp_us = static_cast<uint64_t>(esp_timer_get_time()),
            .value        = voltage,
        };

        if (xQueueSend(destinationQueue, &topic, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Queue cheia — leitura descartada");
        }
    }
}

float VoltageProducer::randomWalk(float current, float maxDelta,
                                   float minVal,  float maxVal)
{
    float r    = (float)esp_random() / (float)UINT32_MAX * 2.0f - 1.0f;
    float next = current + r * maxDelta;
    if (next < minVal) next = minVal;
    if (next > maxVal) next = maxVal;
    return next;
}

float VoltageProducer::randomFloatInRange(float minVal, float maxVal)
{
    float r = (float)esp_random() / (float)UINT32_MAX;
    return minVal + r * (maxVal - minVal);
}