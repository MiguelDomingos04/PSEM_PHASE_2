#include "CANRXProducer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include <cstring>

#define TAG "CANRXProducer"

CANRXProducer::CANRXProducer()
    : Producer(0x00)
{}

//  setup 
// Igual ao twai_init() do receiver da Phase 1
void CANRXProducer::setup()
{
    // Igual à Phase 1 — queue para ISR → task
    rx_queue = xQueueCreate(16, sizeof(CanFrame));

    gpio_reset_pin((gpio_num_t)CANRX_STANDBY_GPIO); // GPIO para tirar o transceiver do modo standby — diferente do CANTXConsumer
    gpio_set_direction((gpio_num_t)CANRX_STANDBY_GPIO, GPIO_MODE_OUTPUT); // Configura o pino como saída. Isso é necessário para controlar o estado do transceiver CAN, permitindo que o microcontrolador ative ou desative a comunicação CAN conforme necessário. Definir o pino como saída garante que o microcontrolador possa enviar um sinal para o transceiver, controlando seu modo de operação (standby ou ativo) e garantindo uma comunicação eficiente e confiável na rede CAN.
    gpio_set_level((gpio_num_t)CANRX_STANDBY_GPIO, 0); // Define o nível baixo para tirar o transceiver do modo standby

    twai_onchip_node_config_t node_config = {
        .io_cfg.tx          = CANRX_TX_GPIO, // GPIO para o sinal de transmissão CAN
        .io_cfg.rx          = CANRX_RX_GPIO, // GPIO para o sinal de recepção CAN
        .bit_timing.bitrate = 200000,
        .tx_queue_depth     = 5,
    };
    ESP_ERROR_CHECK(twai_new_node_onchip(&node_config, &node_hdl));

    // Registar ISR antes de habilitar. A ISR irá ler os frames recebidos e colocar na queue, e a task principal irá processar os frames da queue. Passamos 'this' como user_ctx para que a ISR possa acessar o handle da task e notificar quando um frame for recebido.
    twai_event_callbacks_t cbs = { 
        .on_rx_done = onRxDone,
    };
    ESP_ERROR_CHECK(twai_node_register_event_callbacks(node_hdl, &cbs, this));
    ESP_ERROR_CHECK(twai_node_enable(node_hdl));

    ESP_LOGI(TAG, "CANRXProducer iniciado — TX=GPIO%d RX=GPIO%d",
             CANRX_TX_GPIO, CANRX_RX_GPIO);
}

//  run 
// Igual ao while(true) do receiver da Phase 1 — bloqueia até receber frame da ISR
void CANRXProducer::run()
{
    CanFrame frame;
    if (xQueueReceive(rx_queue, &frame, portMAX_DELAY) != pdTRUE) return;

    if (frame.id == CAN_SYNC_ID) {
        handleSync(frame);
    } else {
        handleTopic(frame);
    }
}

//  ISR callback 
bool IRAM_ATTR CANRXProducer::onRxDone(twai_node_handle_t handle,
                                        const twai_rx_done_event_data_t *edata,
                                        void *user_ctx)
{
    CANRXProducer *self   = static_cast<CANRXProducer*>(user_ctx);
    BaseType_t must_yield = pdFALSE;

    uint8_t recv_buff[8];
    twai_frame_t rx_frame = {
        .buffer     = recv_buff,
        .buffer_len = sizeof(recv_buff),
    };

    if (twai_node_receive_from_isr(handle, &rx_frame) == ESP_OK) {
        CanFrame frame;
        frame.id       = rx_frame.header.id;
        frame.data_len = rx_frame.buffer_len;
        memcpy(frame.data, recv_buff, rx_frame.buffer_len);

        xQueueSendFromISR(self->rx_queue, &frame, &must_yield);
    }

    return must_yield == pdTRUE;
}

//  handleSync 
// Atualiza o timestamp base com o valor recebido do sender
void CANRXProducer::handleSync(const CanFrame& frame)
{
    memcpy(&base_timestamp_us, frame.data, sizeof(uint64_t));
    ESP_LOGI(TAG, "Sync recebido — base_ts=%llu", base_timestamp_us);
}

//  handleTopic 
// Igual ao switch(frame.id) da Phase 1 mas reconstrói topic_t
// em vez de fazer ESP_LOGI — coloca na queue central do TelemetryManager
void CANRXProducer::handleTopic(const CanFrame& frame)
{
    if (frame.data_len < 4) return;

    uint16_t delta_us;
    uint16_t value_scaled;
    memcpy(&delta_us,     frame.data,     sizeof(uint16_t));
    memcpy(&value_scaled, frame.data + 2, sizeof(uint16_t));

    topic_t topic = {
        .producer_id  = (uint8_t)frame.id,
        .timestamp_us = base_timestamp_us + delta_us,
        .value        = unscaleValue((uint8_t)frame.id, value_scaled),
    };

    if (xQueueSend(destinationQueue, &topic, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Queue cheia — frame CAN descartada");
    }

    ESP_LOGI(TAG, "CAN RX — id=0x%02X delta=%u val=%.2f",
             topic.producer_id, delta_us, topic.value);
}

// unscaleValue 
// Inverte a escala do sender para recuperar o valor físico original
float CANRXProducer::unscaleValue(uint8_t producer_id, uint16_t scaled)
{
    switch (producer_id) {
        case PRODUCER_ID_VOLTAGE:
            return scaled / 100.0f;

        case PRODUCER_ID_CURRENT:
            return scaled / 100.0f;

        case PRODUCER_ID_TEMP:
            return (scaled / 10.0f) - 40.0f;

        case PRODUCER_ID_SPEED:
            return scaled / 100.0f;

        case PRODUCER_ID_STEERING:
            return (scaled / 100.0f) - 180.0f;

        case PRODUCER_ID_CPU_USAGE:
            return scaled / 10.0f;    // 0–1000 → 0–100%

        case PRODUCER_ID_QUEUE_SIZE:
            return (float)scaled;     // já é inteiro

        case PRODUCER_ID_TICK_HEALTH:
            return scaled / 10.0f;   // 0–10000 → ms

        default:
            return (float)scaled;
    }
}