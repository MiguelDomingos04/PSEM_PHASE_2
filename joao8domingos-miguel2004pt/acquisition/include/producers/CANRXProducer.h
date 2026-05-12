#pragma once
#include "Producer.h"
#include "topic.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "esp_attr.h"
#include "freertos/queue.h"
#include <cstdint>

#define CAN_SYNC_ID        0x000
#define CANRX_TX_GPIO      40
#define CANRX_RX_GPIO      39
#define CANRX_STANDBY_GPIO 3

class CANRXProducer : public Producer {
private:
    twai_node_handle_t node_hdl          = nullptr;
    uint64_t           base_timestamp_us = 0;
    QueueHandle_t      rx_queue          = nullptr;

    // Igual ao can_frame_t da Phase 1
    struct CanFrame {
        uint32_t id;
        uint8_t  data[8];
        uint8_t  data_len;
    };

    // ISR callback — static igual ao twai_rx_cb da Phase 1
    static bool IRAM_ATTR onRxDone(twai_node_handle_t handle, const twai_rx_done_event_data_t *edata, void *user_ctx);

    void  handleSync(const CanFrame& frame);
    void  handleTopic(const CanFrame& frame);
    float unscaleValue(uint8_t producer_id, uint16_t scaled);

public:
    CANRXProducer();

    void setup() override;
    void run()   override;
};