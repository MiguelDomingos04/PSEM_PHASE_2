#include "producers/CurrentProducer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"
#include <cstdint>

#define TAG          "CurrentProducer"
#define SAMPLE_MS    100
#define MIN_CURRENT  0.0f
#define MAX_CURRENT  30.0f
#define MAX_DELTA    2.0f
#define ADC_CHANNEL  ADC_CHANNEL_4   // GPIO5
#define SAMPLE_FREQ  20000
#define FRAME_SIZE   256
#define DMA_BUF_SIZE (FRAME_SIZE * 4)

//  Regra de três para conversão ADC → corrente 
// Ajustar com os valores reais do sensor na sessão presencial
// Exemplo: sensor de 0-30A com saída 0-3V
//   0A   → 0V    → raw=0
//   30A  → 3.0V  → raw=~3745
#define MAX_CURRENT_A  30.0f
#define V_REF          3.1f

// Construtor. Inicializa a corrente atual com um número aleatório dentro do intervalo realista.
CurrentProducer::CurrentProducer(bool useMockData): Producer(PRODUCER_ID_CURRENT), useMockData(useMockData)
{
    current_value = randomFloatInRange(MIN_CURRENT, MAX_CURRENT);
}

// Setup. Se useMockData for true, não há hardware a inicializar — este producer usa mock data. Caso contrário, inicializa o ADC.
void CurrentProducer::setup()
{
    if (useMockData) {
        ESP_LOGI(TAG, "CurrentProducer iniciado em modo MOCK — valor inicial: %.2fA",
                 current_value);
    } else {
        setupADC();
        ESP_LOGI(TAG, "CurrentProducer iniciado em modo HARDWARE");
    }
}

// Run. Se useMockData for true, gera um novo valor com random walk. Caso contrário, bloqueia até a ISR notificar que há um frame pronto, e então lê o valor do ADC. Em seguida, cria umtopic_t1 para cada amostra do frame e envia para a queue.
void CurrentProducer::run()
{
    if (useMockData) {
        current_value = randomWalk(current_value, MAX_DELTA, MIN_CURRENT, MAX_CURRENT);

       topic_t1 topic = {
            .producer_id  = producerId,
            .timestamp_us = static_cast<uint64_t>(esp_timer_get_time()),
            .value        = current_value,
        };

        if (xQueueSend(destinationQueue, &topic, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Queue cheia — leitura descartada");
        }

        ESP_LOGI(TAG, "current=%.2fA [MOCK]", current_value);
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));

    } else {
        task_handle = xTaskGetCurrentTaskHandle(); // Guarda handle da task para a ISR saber quem acordar
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // Bloqueia até a ISR notificar que há um frame pronto
        readADC(); // Envia umtopic_t1 por amostra do frame
    }
}

bool IRAM_ATTR CurrentProducer::onConvDone(adc_continuous_handle_t handle, const adc_continuous_evt_data_t *edata, void *user_data)
{
    CurrentProducer *self = static_cast<CurrentProducer*>(user_data);
    BaseType_t must_yield = pdFALSE;
    vTaskNotifyGiveFromISR(self->task_handle, &must_yield);
    return must_yield == pdTRUE;
}

void CurrentProducer::setupADC()
{
    adc_continuous_handle_cfg_t handle_cfg = {
        .max_store_buf_size = DMA_BUF_SIZE,     // Define o tamanho máximo do buffer de armazenamento do ADC contínuo, que é usado para armazenar os dados convertidos antes de serem processados pela task principal. O valor DMA_BUF_SIZE é calculado como FRAME_SIZE multiplicado por 4, garantindo que haja espaço suficiente para armazenar vários frames de dados do ADC, permitindo uma operação eficiente e contínua do ADC sem perda de dados.
        .conv_frame_size    = FRAME_SIZE,       // Define o tamanho de cada frame de dados que o ADC contínuo irá enviar para o buffer de armazenamento. O valor FRAME_SIZE é definido como 256 bytes, o que significa que cada frame conterá um número específico de amostras do ADC, dependendo do formato de saída dos dados. Esta configuração é crucial para garantir que a task principal possa processar os dados do ADC em blocos gerenciáveis, facilitando a leitura e o envio dos dados para a queue.
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&handle_cfg, &adc_handle));

    adc_digi_pattern_config_t pattern = {
        .atten     = ADC_ATTEN_DB_12,    // Atenuação de 12dB para medir até ~3.1V no GPIO5 (após o divisor de tensão)
        .channel   = ADC_CHANNEL,        // Canal ADC a ser configurado para leitura. Neste caso, ADC_CHANNEL_4, que corresponde ao GPIO5. A configuração deste canal é essencial para garantir que o ADC leia os dados do pino correto onde o sensor de corrente está conectado, permitindo que as leituras de corrente sejam obtidas corretamente.
        .unit      = ADC_UNIT_1,         // Define a unidade ADC a ser usada para a conversão. ADC_UNIT_1 indica que a ADC1 será usada para as conversões, enquanto a ADC2 ficará inativa. Esta configuração é importante para evitar conflitos, especialmente porque a ADC2 é compartilhada com o Wi-Fi no ESP32, e usar a ADC1 garante uma operação mais estável e confiável para as leituras do sensor de corrente.
        .bit_width = ADC_BITWIDTH_12,    // Define a resolução da conversão ADC. ADC_BITWIDTH_12 define uma resolução de 12 bits, o que significa que os valores convertidos podem variar de 0 a 4095. Esta configuração é adequada para a maioria dos sensores de corrente, proporcionando uma boa precisão nas leituras, e é compatível com a maioria dos sensores disponíveis no mercado.
    };

    adc_continuous_config_t dig_cfg = {};
    dig_cfg.sample_freq_hz = SAMPLE_FREQ;      // Define a frequência de amostragem do ADC contínuo, ou seja, quantas vezes por segundo o ADC irá ler o valor do canal e enviar os dados para o buffer de armazenamento. Neste caso, 20.000 amostras por segundo, o que é adequado para monitorar variações rápidas na corrente, especialmente em aplicações onde a corrente pode mudar rapidamente, como em motores ou circuitos de potência.
    dig_cfg.conv_mode      = ADC_CONV_SINGLE_UNIT_1;  // Define omodo de conversão do ADC. ADC_CONV_SINGLE_UNIT_1 indica que apenas a ADC1 será usada para conversão, enquanto a ADC2 ficará inativa. Esta configuração é importante para evitar conflitos com outras funcionalidades do ESP32, como o Wi-Fi, que compartilha a ADC2, garantindo uma operação estável do sistema e permitindo que as leituras de corrente sejam obtidas sem interferências.
    dig_cfg.format         = ADC_DIGI_OUTPUT_FORMAT_TYPE2;  // Define o formato de saída dos dados convertidos pelo ADC. ADC_DIGI_OUTPUT_FORMAT_TYPE2 é um formato específico que inclui informações detalhadas sobre a conversão, como o valor real da amostra, o canal e a unidade de ADC. Este formato é útil para processar os dados do ADC de forma eficiente, permitindo que a task principal extraia facilmente as informações necessárias para calcular a corrente a partir das leituras do ADC, proporcionando uma conversão precisa e eficiente dos dados do sensor de corrente.
    dig_cfg.adc_pattern    = &pattern; // Define o padrão de configuração do canal ADC a ser usado para a conversão. O campo adc_pattern aponta para a estrutura pattern, quecontém as configurações específicas do canal, como atenuação, canal, unidade e resolução. Esta configuração é essencial para garantir que o ADC leia os dados corretamente do sensor de corrente conectado ao GPIO5, proporcionando leituras de corrente precisas e confiáveis.
    dig_cfg.pattern_num    = 1;        // Define o número de canais ADC que serão usados. Neste caso, estamos a usar apenas um canal, então definimos como 1. Esta configuração é importante para informar ao ADC contínuo quantos canais estão configurados e devem ser amostrados, garantindo que o ADC funcione corretamente e envie os dados para o buffer de armazenamento conforme esperado.

    /*
    adc_continuous_config_t dig_cfg = {
        .sample_freq_hz = SAMPLE_FREQ,      // Define a frequência de amostragem do ADC contínuo, ou seja, quantas vezes por segundo o ADC irá ler o valor do canal e enviar os dados para o buffer de armazenamento. Neste caso, 20.000 amostras por segundo, o que é adequado para monitorar variações rápidas na corrente, especialmente em aplicações onde a corrente pode mudar rapidamente, como em motores ou circuitos de potência.
        .conv_mode      = ADC_CONV_SINGLE_UNIT_1,  // Define o modo de conversão do ADC. ADC_CONV_SINGLE_UNIT_1 indica que apenas a ADC1 será usada para conversão, enquanto a ADC2 ficará inativa. Esta configuração é importante para evitar conflitos com outras funcionalidades do ESP32, como o Wi-Fi, que compartilha a ADC2, garantindo uma operação estável do sistema e permitindo que as leituras de corrente sejam obtidas sem interferências.
        .format         = ADC_DIGI_OUTPUT_FORMAT_TYPE2,  // Define o formato de saída dos dados convertidos pelo ADC. ADC_DIGI_OUTPUT_FORMAT_TYPE2 é um formato específico que inclui informações detalhadas sobre a conversão, como o valor real da amostra, o canal e a unidade de ADC. Este formato é útil para processar os dados do ADC de forma eficiente, permitindo que a task principal extraia facilmente as informações necessárias para calcular a corrente a partir das leituras do ADC, proporcionando uma conversão precisa e eficiente dos dados do sensor de corrente.
        .adc_pattern    = &pattern, // Define o padrão de configuração do canal ADC a ser usado para a conversão. O campo adc_pattern aponta para a estrutura pattern, que contém as configurações específicas do canal, como atenuação, canal, unidade e resolução. Esta configuração é essencial para garantir que o ADC leia os dados corretamente do sensor de corrente conectado ao GPIO5, proporcionando leituras de corrente precisas e confiáveis.
        .pattern_num    = 1,        // Define o número de canais ADC que serão usados. Neste caso, estamos a usar apenas um canal, então definimos como 1. Esta configuração é importante para informar ao ADC contínuo quantos canais estão configurados e devem ser amostrados, garantindo que o ADC funcione corretamente e envie os dados para o buffer de armazenamento conforme esperado.
    };
    */
    ESP_ERROR_CHECK(adc_continuous_config(adc_handle, &dig_cfg));

    adc_continuous_evt_cbs_t cbs = { .on_conv_done = onConvDone };
    ESP_ERROR_CHECK(adc_continuous_register_event_callbacks(adc_handle, &cbs, this));
    ESP_ERROR_CHECK(adc_continuous_start(adc_handle));
}

// readADC — umtopic_t1 por amostra 
void CurrentProducer::readADC()
{
    uint8_t  buf[FRAME_SIZE];
    uint32_t out_len = 0;

    esp_err_t ret = adc_continuous_read(adc_handle, buf, FRAME_SIZE, &out_len, 0); // Lê um frame de dados do buffer DMA do ADC contínuo e copia-o para o array "buf". "out_len" recebe o número de bytes efetivamente lidos. O timeout é definido como 0, o que significa que a função retornará imediatamente se não houver dados disponíveis, evitando bloqueios desnecessários. Se a leitura for bem-sucedida, os dados convertidos do ADC estarão disponíveis no array "buf", e "out_len" indicará quantos bytes foram lidos, permitindo que a task principal processe os dados do ADC de forma eficiente.
    if (ret != ESP_OK) return;

    uint32_t n = out_len / SOC_ADC_DIGI_RESULT_BYTES; // O número de amostras lidas é calculado dividindo o comprimento dos dados lidos (out_len) pelo número de bytes que cada amostra do ADC ocupa no formato de saída configurado (SOC_ADC_DIGI_RESULT_BYTES). Este cálculo é necessário para determinar quantas amostras do ADC foram lidas e estão disponíveis no buffer "buf" para processamento. O valor SOC_ADC_DIGI_RESULT_BYTES é definido pelo formato de saída do ADC, e dividir out_len por esse valor fornece o número total de amostras que podem ser processadas a partir dos dados lidos, permitindo que a task principal itere sobre cada amostra do ADC e crie umtopic_t1 para cada uma delas.

    for (uint32_t i = 0; i < n; i++) {
        adc_digi_output_data_t *p = (adc_digi_output_data_t *)&buf[i * SOC_ADC_DIGI_RESULT_BYTES]; // A função adc_digi_output_data_t é uma estrutura que representa o formato de saída dos dados convertidos pelo ADC. O ponteiro "p" é calculado para apontar para a posição correta no buffer "buf" onde os dados da amostra atual estão armazenados. O índice "i" é multiplicado pelo número de bytes por amostra (SOC_ADC_DIGI_RESULT_BYTES) para calcular o deslocamento correto no buffer, permitindo que a task principal acesse os dados de cada amostra do ADC de forma eficiente e precisa. O valor p->type2.data contém o valor bruto da amostra do ADC, que pode ser convertido para corrente usando a função rawToCurrent(), proporcionando leituras de corrente precisas a partir dos dados do ADC.

        float current_a = rawToCurrent((float)p->type2.data); // A função rawToCurrent() é usada para converter o valor bruto da amostra do ADC (p->type2.data) para corrente em amperes usando uma regra de três simples. O valor bruto do ADC é passado como um argumento para a função, que realiza os cálculos necessários para obter a corrente correspondente. A função leva em consideração a resolução do ADC, a tensão de referência e o valor máximo de corrente para fornecer uma conversão precisa dos dados do ADC para corrente, permitindo que a task principal obtenha leituras de corrente realistas a partir dos dados do ADC.

       topic_t1 topic = {
            .producer_id  = producerId,
            .timestamp_us = static_cast<uint64_t>(esp_timer_get_time()),
            .value        = current_a,
        };

        if (xQueueSend(destinationQueue, &topic, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Queue cheia — leitura descartada");
        }
    }
}

//  Regra de três — raw ADC → corrente em amperes 
// Ajustar MAX_CURRENT_A com o sensor real na sessão presencial
float CurrentProducer::rawToCurrent(float raw)
{
    // raw está entre 0 e 4095
    // tensão está entre 0 e V_REF
    // corrente está entre 0 e MAX_CURRENT_A
    // regra de três: current = (raw / 4095) * MAX_CURRENT_A
    return (raw / 4095.0f) * MAX_CURRENT_A;
}

float CurrentProducer::randomWalk(float current, float maxDelta,
                                   float minVal,  float maxVal)
{
    float r    = (float)esp_random() / (float)UINT32_MAX * 2.0f - 1.0f;
    float next = current + r * maxDelta;
    if (next < minVal) next = minVal;
    if (next > maxVal) next = maxVal;
    return next;
}

float CurrentProducer::randomFloatInRange(float minVal, float maxVal)
{
    float r = (float)esp_random() / (float)UINT32_MAX;
    return minVal + r * (maxVal - minVal);
}