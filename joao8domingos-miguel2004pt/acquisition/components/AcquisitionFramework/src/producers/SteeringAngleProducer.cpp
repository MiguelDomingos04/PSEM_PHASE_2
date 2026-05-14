// ------------------>IMPORTANTE<------------------
// NOTA: Este producer não inicializa o barramento SPI2 — assume que o LCDConsumer
// já o fez. Por isso, o LCDConsumer deve ser registado no TelemetryManager antes
// deste producer ser inicializado. 


#include "producers/SteeringAngleProducer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "driver/spi_master.h"
#include <cstdint>
#include "time_of_day.h"

#define TAG           "SteeringAngleProducer"
#define SAMPLE_MS     100 // frequência de amostragem de 10 Hz — o AMT22B é rápido o suficiente para fornecer leituras em tempo real do ângulo de direção, e uma frequência de 10 Hz é adequada para capturar as mudanças no ângulo de direção sem gerar um volume excessivo de dados para processar. Esta frequência permite que o sistema responda rapidamente às mudanças na direção do veículo, proporcionando uma experiência de condução mais fluida e responsiva, enquanto ainda mantém a eficiência do processamento dos dados.

// Pinos SPI — partilha MOSI/SCLK/MISO com o LCD, CS diferente
#define PIN_CLK       12   // SCLK — partilhado com LCD
#define PIN_MOSI      11   // MOSI — partilhado com LCD
#define PIN_MISO      13   // MISO — partilhado com LCD
#define PIN_CS         6   // CS   — diferente do LCD (GPIO10)

// Configuração do AMT22B 
#define SPI_CLOCK_HZ  125000
#define AMT22B_BITS   14. // resolução de 14 bits → 16384 posições (0–16383) para 360° de rotação, ou seja, ~0.022° por bit. O datasheet do AMT22B especifica que a frequência máxima de clock para leitura é de 500 kHz, mas para garantir uma comunicação estável e confiável, especialmente num ambiente com ruído elétrico como um carro, é recomendado usar uma frequência mais baixa, como 125 kHz. Esta frequência é suficientemente rápida para capturar as mudanças no ângulo de direção em tempo real, enquanto minimiza o risco de erros de comunicação devido a interferências ou limitações do hardware.
//#define AMT22B_MAX_VAL ((1 << AMT22B_BITS) - 1)  // 16383
#define AMT22B_MAX_VAL ((1 << (int)AMT22B_BITS) - 1)

// Mock data
#define MIN_ANGLE    -180.0f
#define MAX_ANGLE     180.0f
#define MAX_DELTA       5.0f

// SteeringAngleProducer — lê o sensor de ângulo de direção AMT22B via SPI
SteeringAngleProducer::SteeringAngleProducer(bool useMockData)
    : Producer(PRODUCER_ID_STEERING), useMockData(useMockData)
{
    current_value = randomFloatInRange(MIN_ANGLE, MAX_ANGLE);
}

// setup. Se useMockData for true, não há hardware a inicializar — este producer usa mock data. Caso contrário, inicializa o SPI.
void SteeringAngleProducer::setup()
{
    if (useMockData) {
        ESP_LOGI(TAG, "SteeringAngleProducer iniciado em modo MOCK — valor inicial: %.2f°",
                 current_value);
    } else {
        setupSPI();
        ESP_LOGI(TAG, "SteeringAngleProducer iniciado em modo HARDWARE");
    }
}

// run. Se useMockData for true, gera um novo valor com random walk. Caso contrário, lê o valor do sensor via SPI. Em seguida, cria umtopic_t1 e envia para a queue.
void SteeringAngleProducer::run()
{
    float value;

    if (useMockData) {
        current_value = randomWalk(current_value, MAX_DELTA, MIN_ANGLE, MAX_ANGLE);
        value = current_value;
    } else {
        value = readAngle();
    }

   topic_t1 topic = {
        .producer_id  = producerId,
        .timestamp_us = static_cast<uint64_t>(TimeReader::getCurrentTimeUs()),
        .value        = value,
    };

    if (xQueueSend(destinationQueue, &topic, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Queue cheia — leitura descartada");
    }

    ESP_LOGI(TAG, "steering=%.2f° [%s]", value, useMockData ? "MOCK" : "HW");

    vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
}

// setupSPI 
// Igual à Phase 1 — só o CS é diferente (GPIO6 em vez de GPIO10)
// Não inicializa o barramento SPI2 porque o LCDConsumer já o fez
void SteeringAngleProducer::setupSPI()
{
    spi_device_interface_config_t dev = {};
    dev.clock_speed_hz = SPI_CLOCK_HZ;  // 125 kHz conforme datasheet do AMT22B
        dev.mode           = 0;             // Modo SPI 0 (CPOL=0, CPHA=0)
        dev.spics_io_num   = PIN_CS;        // CS = GPIO6
        dev.queue_size = 1;

    /*
    spi_device_interface_config_t dev = {
        .clock_speed_hz = SPI_CLOCK_HZ,  // 125 kHz conforme datasheet do AMT22B
        .mode           = 0,             // Modo SPI 0 (CPOL=0, CPHA=0)
        .spics_io_num   = PIN_CS,        // CS = GPIO6
        .queue_size     = 1,
    };
    */
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &spi_handle)); // Adiciona o dispositivo SPI à bus

    ESP_LOGI(TAG, "AMT22B iniciado — CS=GPIO%d @ %dHz", PIN_CS, SPI_CLOCK_HZ);
}

// readAngle 
// Igual à Phase 1 — transação de 16 bits, SPI_SWAP_DATA_RX, checksum K0/K1
float SteeringAngleProducer::readAngle()
{
    uint16_t tx = 0x0000;  // envia zeros — o sensor só precisa do clock
    uint16_t rx = 0;       // buffer para receber os dados

    spi_transaction_t t = { // Configuração da transação SPI
        .length    = 16,
        .tx_buffer = &tx,
        .rx_buffer = &rx,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(spi_handle, &t));

    // Corrigir endianness — igual à Phase 1
    rx = SPI_SWAP_DATA_RX(rx, 16);

    // Verificar checksum K0/K1
    if (!amt22bCheck(rx)) {
        ESP_LOGW(TAG, "Checksum falhou (0x%04X)", rx);
        return -999.0f;  // valor inválido
    }

    // Extrair posição — bits [15:2]
    uint16_t raw = rx & 0x3FFF;

    // Converter raw → graus [0°, 360°]
    float angle_deg = ((float)raw / (float)AMT22B_MAX_VAL) * 360.0f;

    // Converter para convenção de direção [-180°, +180°]
    if (angle_deg > 180.0f) {
        angle_deg -= 360.0f;
    }

    return angle_deg;
}

//  amt22bCheck 
// Igual à Phase 1 — paridade ímpar K0/K1
// O AMT22B inclui dois bits de paridade (K0 e K1) para verificar a integridade dos dados. K0 é calculado como a paridade ímpar dos bits de posição nas posições ímpares (1, 3, 5, ..., 15), enquanto K1 é calculado como a paridade ímpar dos bits de posição nas posições pares (0, 2, 4, ..., 14). A função amt22bCheck() implementa esta verificação de paridade para garantir que os dados recebidos do sensor sejam válidos. Se a verificação falhar, a função retorna false, indicando que os dados podem estar corrompidos ou que houve um erro de comunicação.
bool SteeringAngleProducer::amt22bCheck(uint16_t resp)
{
    uint8_t k1 = ((resp >> 15) ^ (resp >> 13) ^ (resp >> 11) ^
                  (resp >>  9) ^ (resp >>  7) ^ (resp >>  5) ^
                  (resp >>  3) ^ (resp >>  1)) & 0x01;

    uint8_t k0 = ((resp >> 14) ^ (resp >> 12) ^ (resp >> 10) ^
                  (resp >>  8) ^ (resp >>  6) ^ (resp >>  4) ^
                  (resp >>  2) ^ (resp >>  0)) & 0x01;

    return (k1 == 1) && (k0 == 1);
}

float SteeringAngleProducer::randomWalk(float current, float maxDelta,
                                         float minVal,  float maxVal)
{
    float r    = (float)esp_random() / (float)UINT32_MAX * 2.0f - 1.0f;
    float next = current + r * maxDelta;
    if (next < minVal) next = minVal;
    if (next > maxVal) next = maxVal;
    return next;
}

float SteeringAngleProducer::randomFloatInRange(float minVal, float maxVal)
{
    float r = (float)esp_random() / (float)UINT32_MAX;
    return minVal + r * (maxVal - minVal);
}