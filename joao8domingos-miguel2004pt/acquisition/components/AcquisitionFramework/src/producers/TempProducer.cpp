#include "producers/TempProducer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"
#include <cstdint>
#include <cmath>

#define TAG          "TempProducer"
#define SAMPLE_MS    100
#define MIN_TEMP     20.0f
#define MAX_TEMP     80.0f
#define MAX_DELTA    1.0f
#define ADC_CHANNEL  ADC_CHANNEL_6   // GPIO7
#define SAMPLE_FREQ  20000
#define FRAME_SIZE   256
#define DMA_BUF_SIZE (FRAME_SIZE * 4)

//  Steinhart-Hart 
// A fórmula de Steinhart-Hart é uma equação empírica usada para converter a resistência de um termistor em temperatura. Ela é amplamente utilizada para termistores NTC (Negative Temperature Coefficient), que têm uma resistência que diminui com o aumento da temperatura. A fórmula é dada por: 
// T = 1 / (A + B*ln(R) + C*(ln(R))^3) - 273.15
// Onde:
// - T é a temperatura em graus Celsius.
// - R é a resistência do termistor, que pode ser calculada a partir do valor bruto do ADC usando a tensão de referência e a resistência de referência do divisor.
// - A, B e C são os coeficientes específicos do termistor, determinados experimentalmente. Esses coeficientes são usados para ajustar a curva de resposta do termistor e obter uma conversão precisa da resistência para temperatura. A fórmula de Steinhart-Hart é uma aproximação que fornece uma boa precisão numa ampla faixa de temperaturas, tornando-a ideal para aplicações que exigem medições de temperatura precisas
#define SH_A  1.009249522e-3f      // Coeficiente A da fórmula de Steinhart-Hart, que é uma constante específica do termistor usado. Este coeficiente é determinado experimentalmente e representa a parte linear da resposta do termistor à temperatura. Ele é usado na fórmula para calcular a temperatura a partir da resistência do termistor, e seu valor é crucial para obter leituras de temperatura precisas.
#define SH_B  2.378405444e-4f      // Coeficiente B da fórmula de Steinhart-Hart, que é uma constante específica do termistor usado. Este coeficiente representa a parte logarítmica da resposta do termistor à temperatura. Assim como o coeficiente A, o valor de B é determinado experimentalmente e é essencial para calcular a temperatura corretamente a partir da resistência do termistor.
#define SH_C  2.019202697e-7f      // Coeficiente C da fórmula de Steinhart-Hart, que é uma constante específica do termistor usado. Este coeficiente representa a parte cúbica logarítmica da resposta do termistor à temperatura. O valor de C é determinado experimentalmente e é necessário para obter uma aproximação precisa da curva de resposta do termistor, especialmente numa faixa de temperatura mais ampla. Junto com os coeficientes A e B, o coeficiente C permite calcular a temperatura a partir da resistência do termistor usando a fórmula de Steinhart-Hart.
#define R_REF 10000.0f       // resistência de referência do divisor (10kΩ)
#define V_REF 3.1f           // tensão de referência do ADC


// Construtor. Inicializa a temperatura atual com um número aleatório dentro do intervalo realista.
TempProducer::TempProducer(bool useMockData)
    : Producer(PRODUCER_ID_TEMP), useMockData(useMockData)
{
    current_value = randomFloatInRange(MIN_TEMP, MAX_TEMP);
}

// Setup. Se useMockData for true, não há hardware a inicializar — este producer usa mock data. Caso contrário, inicializa o ADC.
void TempProducer::setup()
{
    if (useMockData) {
        ESP_LOGI(TAG, "TempProducer iniciado em modo MOCK — valor inicial: %.2fC",
                 current_value);
    } else {
        setupADC();
        ESP_LOGI(TAG, "TempProducer iniciado em modo HARDWARE");
    }
}

// Run. Se useMockData for true, gera um novo valor com random walk. Caso contrário, bloqueia até a ISR notificar que há um frame pronto, e então lê o valor do ADC. Em seguida, cria umtopic_t1 para cada amostra do frame e envia para a queue.
void TempProducer::run()
{
    if (useMockData) {
        current_value = randomWalk(current_value, MAX_DELTA, MIN_TEMP, MAX_TEMP);

       topic_t1 topic = {
            .producer_id  = producerId,
            .timestamp_us = static_cast<uint64_t>(esp_timer_get_time()),
            .value        = current_value,
        };

        if (xQueueSend(destinationQueue, &topic, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Queue cheia — leitura descartada");
        }

        ESP_LOGI(TAG, "temp=%.2fC [MOCK]", current_value);
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));

    } else {
        // Guarda handle da task para a ISR saber quem acordar
        task_handle = xTaskGetCurrentTaskHandle(); // A função xTaskGetCurrentTaskHandle() é usada para obter o handle da task atual, que é necessário para a ISR (Interrupt Service Routine) notificar a task correta quando um frame de dados do ADC estiver pronto. O handle da task é armazenado na variável membro task_handle da classe TempProducer, permitindo que a ISR use esse handle para acordar a task específica que está esperando pelos dados do ADC, garantindo uma comunicação eficiente entre a ISR e a task.

        // A função ulTaskNotifyTake() é usada para bloquear a task atual até que a ISR (Interrupt Service Routine) notifique que um frame de dados do ADC está pronta. A função espera por uma notificação, e quando a ISR chama vTaskNotifyGiveFromISR() para sinalizar que os dados estão prontos, ulTaskNotifyTake() retorna, permitindo que a task continue sua execução e processe os dados do ADC. O uso de ulTaskNotifyTake() é uma forma eficiente de sincronização entre a ISR e a task, evitando o uso de polling e economizando recursos do sistema.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Envia umtopic_t1 por amostra do frame
        readADC();
    }
}

// ISR callback 
bool IRAM_ATTR TempProducer::onConvDone(adc_continuous_handle_t handle, const adc_continuous_evt_data_t *edata, void *user_data)
{
    TempProducer *self    = static_cast<TempProducer*>(user_data); // recupera o ponteiro para a instância da classe TempProducer a partir do parâmetro user_data, que foi passado quando a ISR foi registrada. Isso permite que a ISR acesse os membros da classe, como task_handle, para notificar a task correta quando um frame de dados do ADC estiver pronto.
    BaseType_t must_yield = pdFALSE; // variável para indicar se a task deve ser acordada imediatamente após a notificação. A função vTaskNotifyGiveFromISR() pode definir must_yield como pdTRUE se a task que está sendo notificada tiver uma prioridade maior do que a task atual, indicando que o sistema deve realizar uma troca de contexto imediatamente para permitir que a task notificada execute sem atrasos desnecessários.
    vTaskNotifyGiveFromISR(self->task_handle, &must_yield); // notifica a task associada ao handle armazenado em self->task_handle que um frame de dados do ADC está pronto. A função vTaskNotifyGiveFromISR() é usada para enviar uma notificação a partir de uma ISR, e o parâmetro must_yield é usado para indicar se a task notificada deve ser acordada imediatamente, dependendo de sua prioridade em relação à task atual.
    return must_yield == pdTRUE; // retorna true se a task notificada deve ser acordada imediatamente, o que pode ocorrer se a task tiver uma prioridade mais alta do que a task atual. Isso permite que o sistema realize uma troca de contexto eficiente, garantindo que a task que está esperando pelos dados do ADC possa processá-los o mais rápido possível após a notificação da ISR.
}

void TempProducer::setupADC()
{
    adc_continuous_handle_cfg_t handle_cfg = {
        .max_store_buf_size = DMA_BUF_SIZE,       // Define o tamanho máximo do buffer de armazenamento do ADC contínuo, que é usado para armazenar os dados convertidos antes de serem processados. O valor DMA_BUF_SIZE é definido como FRAME_SIZE * 4, o que significa que o buffer pode armazenar até 4 frames de dados do ADC. Isso permite que o ADC contínuo continue a converter e armazenar dados mesmo enquanto a task principal está processando um frame, evitando perda de dados e garantindo uma operação suave do sistema.
        .conv_frame_size    = FRAME_SIZE,         // Define o tamanho de cada frame de conversão do ADC contínuo, que é a quantidade de bytes que o ADC irá converter e enviar para o buffer de armazenamento antes de notificar a task principal. O valor FRAME_SIZE é definido como 256 bytes, o que significa que cada frame conterá os dados de várias amostras do ADC, dependendo do formato de saída configurado. O tamanho do frame é importante para equilibrar a latência e a eficiência do processamento dos dados do ADC, permitindo que a task principal processe os dados em blocos gerenciáveis.
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&handle_cfg, &adc_handle));

    adc_digi_pattern_config_t pattern = {
        .atten     = ADC_ATTEN_DB_12,      // Define a atenuação do canal ADC, que determina a faixa de tensão que pode ser medida. Neste caso, ADC_ATTEN_DB_12 permite medir até aproximadamente 3.1V, o que é adequado para um sensor de temperatura conectado ao GPIO7 com um divisor de tensão.
        .channel   = ADC_CHANNEL,          // Define o canal ADC a ser usado para a conversão. ADC_CHANNEL é definido como ADC_CHANNEL_6, que corresponde ao GPIO7 no ESP32. Este é o canal onde o sensor de temperatura está conectado, e a configuração correta do canal é essencial para obter leituras precisas do sensor.
        .unit      = ADC_UNIT_1,           // Define a unidade ADC a ser usada para a conversão. ADC_UNIT_1 indica que a ADC1 será usada, enquanto a ADC2 ficará inativa. A escolha da unidade é importante para evitar conflitos com outras funcionalidades do ESP32, como o Wi-Fi, que compartilha a ADC2.
        .bit_width = ADC_BITWIDTH_12,     // Define a resolução da conversão ADC. ADC_BITWIDTH_12 define uma resolução de 12 bits, o que significa que os valores convertidos podem variar de 0 a 4095. Esta resolução é adequada para a maioria das aplicações de sensores, proporcionando uma boa precisão nas leituras de temperatura.
    };

    adc_continuous_config_t dig_cfg = {};
    dig_cfg.sample_freq_hz = SAMPLE_FREQ;             // Define a frequência de amostr  agem do ADC, ou seja, quantas vezes por segundo o ADC irá ler o valor do canal e enviar os dados para o buffer de armazenamento. Neste caso, SAMPLE_FREQ é definido como 20000, o que significa que o ADC irá amostrar a temperatura 20.000 vezes por segundo, proporcionando uma alta taxa de atualização para as leituras de temperatura.
    dig_cfg.conv_mode      = ADC_CONV_SINGLE_UNIT_1;  // Define o modo de conversão do ADC. ADC_CONV_SINGLE_UNIT_1 indica que apenas a ADC1 será usada para conversão, enquanto a ADC2 ficará inativa. Esta configuração é importante para evitar conflitos com outras funcionalidades do ESP32, como o Wi-Fi, que compartilha a ADC2, garantindo uma operação estável do sistema.
    dig_cfg.format         = ADC_DIGI_OUTPUT_FORMAT_TYPE2; // Define o formato de saída dos dados convertidos pelo ADC. ADC_DIGI_OUTPUT_FORMAT_TYPE2 é um formato específico que inclui informações detalhadas sobre a conversão, como o valor real da amostra, o canal e a unidade de ADC. Este formato é útil para processar os dados do ADC de forma eficiente, permitindo que a task principal extraia facilmente as informações necessárias para calcular a temperatura a partir das leituras do ADC.
    dig_cfg.adc_pattern    = &pattern;                     // Define o padrão de configuração do canal ADC a ser usado para a conversão. O campo adc_pattern aponta para a estrutura pattern, que contém as configurações específicas do canal, como atenuação, canal, unidade e resolução. Esta configuração é essencial para garantir que o ADC leia os dados corretamente do sensor de temperatura conectado ao GPIO7, proporcionando leituras precisas e confiáveis.
    dig_cfg.pattern_num    = 1;                            // Define o número de canais ADC que serão usados para a conversão. Neste caso, pattern_num é definido como 1, indicando que apenas um canal ADC será configurado e usado para a conversão, o que é adequado para esta aplicação, onde apenas um sensor de temperatura está conectado ao GPIO7.

    /*
    adc_continuous_config_t dig_cfg = {
        .sample_freq_hz = SAMPLE_FREQ,             // Define a frequência de amostragem do ADC, ou seja, quantas vezes por segundo o ADC irá ler o valor do canal e enviar os dados para o buffer de armazenamento. Neste caso, SAMPLE_FREQ é definido como 20000, o que significa que o ADC irá amostrar a temperatura 20.000 vezes por segundo, proporcionando uma alta taxa de atualização para as leituras de temperatura.
        .conv_mode      = ADC_CONV_SINGLE_UNIT_1,  // Define o modo de conversão do ADC. ADC_CONV_SINGLE_UNIT_1 indica que apenas a ADC1 será usada para conversão, enquanto a ADC2 ficará inativa. Esta configuração é importante para evitar conflitos com outras funcionalidades do ESP32, como o Wi-Fi, que compartilha a ADC2, garantindo uma operação estável do sistema.
        .format         = ADC_DIGI_OUTPUT_FORMAT_TYPE2, // Define o formato de saída dos dados convertidos pelo ADC. ADC_DIGI_OUTPUT_FORMAT_TYPE2 é um formato específico que inclui informações detalhadas sobre a conversão, como o valor real da amostra, o canal e a unidade de ADC. Este formato é útil para processar os dados do ADC de forma eficiente, permitindo que a task principal extraia facilmente as informações necessárias para calcular a temperatura a partir das leituras do ADC.
        .adc_pattern    = &pattern,                     // Define o padrão de configuração do canal ADC a ser usado para a conversão. O campo adc_pattern aponta para a estrutura pattern, que contém as configurações específicas do canal, como atenuação, canal, unidade e resolução. Esta configuração é essencial para garantir que o ADC leia os dados corretamente do sensor de temperatura conectado ao GPIO7, proporcionando leituras precisas e confiáveis.
        .pattern_num    = 1,                            // Define o número de canais ADC que serão usados para a conversão. Neste caso, pattern_num é definido como 1, indicando que apenas um canal ADC será configurado e usado para a conversão, o que é adequado para esta aplicação, onde apenas um sensor de temperatura está conectado ao GPIO7.
    };
    */
    ESP_ERROR_CHECK(adc_continuous_config(adc_handle, &dig_cfg));

    adc_continuous_evt_cbs_t cbs = { .on_conv_done = onConvDone };
    ESP_ERROR_CHECK(adc_continuous_register_event_callbacks(adc_handle, &cbs, this));
    ESP_ERROR_CHECK(adc_continuous_start(adc_handle));
}

//  readADC — umtopic_t1 por amostra 
void TempProducer::readADC()
{
    uint8_t  buf[FRAME_SIZE];
    uint32_t out_len = 0;

    // Lê um frame de dados do buffer DMA do ADC contínuo e copia-o para o array "buf".
    // "out_len" recebe o número de bytes efetivamente lidos.
    esp_err_t ret = adc_continuous_read(adc_handle, buf, FRAME_SIZE, &out_len, 0); // A função adc_continuous_read() é usada para ler os dados convertidos do ADC contínuo. Ela recebe o handle do ADC, um buffer para armazenar os dados lidos, o tamanho do buffer, um ponteiro para armazenar o comprimento dos dados lidos e um timeout. A função retorna um código de erro (esp_err_t) que indica se a leitura foi bem-sucedida ou se ocorreu algum problema. O buffer buf é preenchido com os dados convertidos do ADC, e out_len é atualizado com o número de bytes realmente lidos. O timeout é definido como 0, o que significa que a função retornará imediatamente se não houver dados disponíveis, evitando bloqueios desnecessários.
    if (ret != ESP_OK) return;

    uint32_t n = out_len / SOC_ADC_DIGI_RESULT_BYTES; // O número de amostras lidas é calculado dividindo o comprimento dos dados lidos (out_len) pelo número de bytes que cada amostra do ADC ocupa no formato de saída configurado (SOC_ADC_DIGI_RESULT_BYTES). Este cálculo é necessário para determinar quantas amostras do ADC foram lidas e estão disponíveis no buffer buf para processamento. O valor SOC_ADC_DIGI_RESULT_BYTES é definido pelo formato de saída do ADC, e dividir out_len por esse valor fornece o número total de amostras que podem ser processadas a partir dos dados lidos.

    for (uint32_t i = 0; i < n; i++) {
        adc_digi_output_data_t *p = (adc_digi_output_data_t *)&buf[i * SOC_ADC_DIGI_RESULT_BYTES]; // A função adc_digi_output_data_t é uma estrutura que representa o formato de saída dos dados convertidos pelo ADC. O ponteiro p é calculado para apontar para a posição correta no buffer buf onde os dados da amostra atual estão armazenados. O índice i é multiplicado pelo número de bytes por amostra (SOC_ADC_DIGI_RESULT_BYTES) para calcular o deslocamento correto no buffer, permitindo que a task principal acesse os dados de cada amostra do ADC de forma eficiente e precisa. O valor p->type2.data contém o valor bruto da amostra do ADC, que pode ser convertido para temperatura usando a fórmula de Steinhart-Hart, proporcionando leituras de temperatura precisas a partir dos dados do ADC.

        float temp_c = steinhartHart((float)p->type2.data); // A função steinhartHart() é usada para converter o valor bruto da amostra do ADC (p->type2.data) para temperatura em graus Celsius usando a fórmula de Steinhart-Hart. O valor bruto do ADC é passado como um argumento para a função, que realiza os cálculos necessários para obter a temperatura correspondente. A função leva em consideração a tensão de referência, a resistência de referência e os coeficientes específicos do termistor para fornecer uma conversão precisa da resistência do termistor (calculada a partir do valor bruto do ADC) para temperatura, permitindo que a task principal obtenha leituras de temperatura realistas a partir dos dados do ADC.

       topic_t1 topic = {
            .producer_id  = producerId,
            .timestamp_us = static_cast<uint64_t>(esp_timer_get_time()),
            .value        = temp_c,
        };

        if (xQueueSend(destinationQueue, &topic, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Queue cheia — leitura descartada");
        }
    }
}

// Steinhart-Hart 
float TempProducer::steinhartHart(float raw)
{
    // O circuito externo já converte R para mV
    // raw ADC → mV → R em ohms
    float mv = (raw / 4095.0f) * 3100.0f;  // 3.1V = 3100mV
    float r  = mv;  // assume 1mV = 1Ω — ajustar com o circuito real. 

    // Aplica a fórmula de Steinhart-Hart para converter a resistência do termistor em temperatura. A função logf() é usada para calcular o logaritmo natural da resistência, que é necessário para a fórmula. Os coeficientes SH_A, SH_B e SH_C são usados para ajustar a curva de resposta do termistor, permitindo uma conversão precisa da resistência para temperatura. O resultado da fórmula é a temperatura em Kelvin, e subtrair 273.15 converte para graus Celsius, fornecendo uma leitura de temperatura realista a partir dos dados do ADC.
    float ln_r   = logf(r); 
    float temp_k = 1.0f / (SH_A + SH_B * ln_r + SH_C * ln_r * ln_r * ln_r);
    return temp_k - 273.15f; // Converte de Kelvin para Celsius
}

float TempProducer::randomWalk(float current, float maxDelta,
                                float minVal,  float maxVal)
{
    float r    = (float)esp_random() / (float)UINT32_MAX * 2.0f - 1.0f;
    float next = current + r * maxDelta;
    if (next < minVal) next = minVal;
    if (next > maxVal) next = maxVal;
    return next;
}

float TempProducer::randomFloatInRange(float minVal, float maxVal)
{
    float r = (float)esp_random() / (float)UINT32_MAX;
    return minVal + r * (maxVal - minVal);
}