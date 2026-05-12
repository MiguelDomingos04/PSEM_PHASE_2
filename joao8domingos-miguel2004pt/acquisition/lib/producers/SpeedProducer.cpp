#include "SpeedProducer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include <cstdint>
#include <cmath>

#define TAG            "SpeedProducer"
#define SAMPLE_MS      50
#define MIN_RPM        0
#define MAX_RPM        12000
#define MAX_DELTA_RPM  500
#define WHEEL_RADIUS_M 0.26f
#define GEAR_RATIO     4.0f
#define CAP_GPIO       4
#define MCPWM_GROUP    0
#define TIMEOUT_MS     2000
#define PULSES_PER_REV 8

// Construtor. Inicializa o RPM atual com um número aleatório dentro do intervalo realista.
SpeedProducer::SpeedProducer(bool useMockData): Producer(PRODUCER_ID_SPEED), useMockData(useMockData)
{
    float r   = (float)esp_random() / (float)UINT32_MAX; // Gera um número aleatório entre 0 e 1 usando esp_random(), que retorna um uint32_t. Dividindo pelo valor máximo de uint32_t, obtemos um float entre 0.0f e 1.0f.
    current_rpm = (int)(r * MAX_RPM); // Escala o número aleatório para o intervalo de RPM realista (0 a MAX_RPM) e converte para int. Isto garante que o RPM inicial seja um valor plausível para um drone, simulando uma condição de operação realista desde o início.
}

// Setup. Se useMockData for true, não há hardware a inicializar — este producer usa mock data. Caso contrário, inicializa o MCPWM para capturar os pulsos do sensor de velocidade.
void SpeedProducer::setup()
{
    if (useMockData) {
        ESP_LOGI(TAG, "SpeedProducer iniciado em modo MOCK — RPM inicial: %d",
                 current_rpm);
    } else {
        setupMCPWM();
        ESP_LOGI(TAG, "SpeedProducer iniciado em modo HARDWARE");
    }
}

void SpeedProducer::run()
{
    float speed_kmh;

    if (useMockData) {
        current_rpm = randomWalkInt(current_rpm, MAX_DELTA_RPM, MIN_RPM, MAX_RPM);
        speed_kmh   = rpmToKmh(current_rpm);

        topic_t topic = {
            .producer_id  = producerId,
            .timestamp_us = static_cast<uint64_t>(esp_timer_get_time()),
            .value        = speed_kmh,
        };

        if (xQueueSend(destinationQueue, &topic, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Queue cheia — leitura descartada");
        }

        ESP_LOGI(TAG, "rpm=%d  speed=%.2f km/h [MOCK]", current_rpm, speed_kmh);
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));

    } else {
        speed_kmh = readSpeed();

        topic_t topic = {
            .producer_id  = producerId,
            .timestamp_us = static_cast<uint64_t>(esp_timer_get_time()),
            .value        = speed_kmh,
        };

        if (xQueueSend(destinationQueue, &topic, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Queue cheia — leitura descartada");
        }

        ESP_LOGI(TAG, "speed=%.2f km/h [HW]", speed_kmh);
    }
}

// ISR callback 
// Igual ao cap_callback da Phase 1 — máquina de estados T1/T2/T3
bool IRAM_ATTR SpeedProducer::capCallback(mcpwm_cap_channel_handle_t chan, const mcpwm_capture_event_data_t *edata, void *user_data)
{
    static uint8_t  state = 0;
    static uint32_t t1    = 0;
    static uint32_t t2    = 0;

    SpeedProducer *self      = static_cast<SpeedProducer*>(user_data);
    BaseType_t     must_yield = pdFALSE;

    switch (state) {
        case 0:
            if (edata->cap_edge == MCPWM_CAP_EDGE_POS) {
                t1 = edata->cap_value;
                state = 1;
            }
            break;
        case 1:
            if (edata->cap_edge == MCPWM_CAP_EDGE_NEG) {
                t2 = edata->cap_value;
                state = 2;
            }
            break;
        case 2:
            if (edata->cap_edge == MCPWM_CAP_EDGE_POS) {
                uint32_t t3 = edata->cap_value;
                CaptureMeas meas = { t3 - t1, t2 - t1 };
                xQueueSendFromISR(self->meas_queue, &meas, &must_yield);
                t1    = t3;
                state = 1;
            }
            break;
    }
    return must_yield == pdTRUE;
}

void SpeedProducer::setupMCPWM()
{
    meas_queue = xQueueCreate(8, sizeof(CaptureMeas)); // Cria uma queue para enviar as medições da ISR para a task principal do producer. A queue pode armazenar até 8 medições, e cada item é do tamanho da estrutura CaptureMeas, que contém os ticks de período e de pulso alto.

    mcpwm_capture_timer_config_t timer_cfg = {
        .group_id = MCPWM_GROUP,                       // Grupo MCPWM a ser usado. O ESP32 tem 2 grupos (0 e 1), cada um com 3 timers. Usamos o grupo 0 para este producer.
        .clk_src  = MCPWM_CAPTURE_CLK_SRC_DEFAULT,     // Fonte de clock padrão para o timer de captura. Normalmente é o APB clock (80 MHz), mas pode variar dependendo da configuração do MCPWM.
    };
    ESP_ERROR_CHECK(mcpwm_new_capture_timer(&timer_cfg, &cap_timer));

    mcpwm_capture_channel_config_t chan_cfg = {
        .gpio_num       = CAP_GPIO,               // GPIO onde o sensor de velocidade está conectado. Este GPIO deve ser capaz de funcionar como entrada para o timer de captura do MCPWM. O GPIO4 é uma escolha comum para este tipo de aplicação.
        .prescale       = 1,                      // Prescaler do timer de captura. Define a divisão do clock para o timer. Com um prescaler de 1, o timer conta a cada pulso do clock (por exemplo, a 80 MHz). Se o prescaler for maior, o timer contará mais lentamente, o que pode ser útil para medir períodos mais longos sem overflow, mas reduz a resolução temporal.
        .flags.pos_edge = true,                  // Captura na borda positiva (subida) do sinal. O sensor de velocidade geralmente gera um pulso para cada evento de medição (por exemplo, cada vez que uma marca no eixo passa pelo sensor), e queremos capturar o tempo desses eventos para calcular a velocidade.
        .flags.neg_edge = true,                  // Captura na borda negativa (descida) do sinal. Capturar tanto a borda positiva quanto a negativa permite medir o período total do sinal e o tempo em que o sinal está alto, o que pode ser útil para calcular a velocidade com base em pulsos de largura variável.
        .flags.pull_up  = true,                  // Ativa o pull-up interno no GPIO de captura. Isso é importante para garantir que o sinal de entrada tenha um nível definido (alto) quando o sensor não estiver ativamente puxando o sinal para baixo. O pull-up ajuda a evitar leituras erráticas causadas por ruído ou flutuações no sinal quando o sensor está inativo.
    };
    ESP_ERROR_CHECK(mcpwm_new_capture_channel(cap_timer, &chan_cfg, &cap_chan));

    mcpwm_capture_event_callbacks_t cbs = { // Registra a função de callback para eventos de captura. A função capCallback será chamada automaticamente pelo driver do MCPWM sempre que ocorrer um evento de captura (borda positiva ou negativa) no canal configurado. O ponteiro user_data é usado para passar uma referência ao objeto SpeedProducer para a ISR, permitindo que a ISR envie as medições para a queue correta.
        .on_cap = capCallback,
    };
    ESP_ERROR_CHECK(mcpwm_capture_channel_register_event_callbacks(
                        cap_chan, &cbs, this));

    ESP_ERROR_CHECK(mcpwm_capture_channel_enable(cap_chan));
    ESP_ERROR_CHECK(mcpwm_capture_timer_enable(cap_timer));
    ESP_ERROR_CHECK(mcpwm_capture_timer_start(cap_timer));
}

float SpeedProducer::readSpeed()
{
    uint32_t resolution;
    ESP_ERROR_CHECK(mcpwm_capture_timer_get_resolution(cap_timer, &resolution)); // Obtém a resolução do timer de captura, que é o número de ticks por segundo. A resolução depende do clock do timer e do prescaler configurado. Por exemplo, se o clock for 80 MHz e o prescaler for 1, a resolução será 80 milhões de ticks por segundo.

    CaptureMeas meas; // Estrutura para armazenar a medição recebida da ISR. Contém o número de ticks do período total (t3 - t1) e o número de ticks do pulso alto (t2 - t1).
    if (xQueueReceive(meas_queue, &meas, pdMS_TO_TICKS(TIMEOUT_MS)) != pdTRUE) { // Tenta receber uma medição da queue. Se não receber dentro do tempo limite (TIMEOUT_MS), assume que não há sinal e retorna 0 km/h. Isso pode acontecer se o carro estiver parado ou se houver um problema com o sensor de velocidade.
        ESP_LOGW(TAG, "Sem sinal — velocidade = 0 km/h");
        return 0.0f;
    }

    // Calcula a velocidade em km/h com base na medição de captura. A fórmula é: RPM = (1 / período) * (60 segundos / minuto) * (1 / PULSES_PER_REV) para obter as rotações por minuto, e depois converte para km/h usando a função rpmToKmh(). O período é calculado dividindo o número de ticks do período total pela resolução do timer para obter o tempo em segundos.
    float period_s = (float)meas.period_ticks / (float)resolution; 
    float freq_hz  = 1.0f / period_s;
    float rpm      = (freq_hz / PULSES_PER_REV) * 60.0f;
    return rpmToKmh((int)rpm);
}

int SpeedProducer::randomWalkInt(int current, int maxDelta,
                                  int minVal,  int maxVal)
{
    float r    = (float)esp_random() / (float)UINT32_MAX * 2.0f - 1.0f;
    int   next = current + (int)(r * (float)maxDelta);
    if (next < minVal) next = minVal;
    if (next > maxVal) next = maxVal;
    return next;
}

float SpeedProducer::rpmToKmh(int rpm)
{
    // Converte RPM para km/h usando a fórmula: speed = (RPM / gear_ratio) * (2 * π * wheel_radius) / 60 * 3.6
    return ((float)rpm / GEAR_RATIO) * (2.0f * 3.14159f * WHEEL_RADIUS_M) / 60.0f * 3.6f;
}