#pragma once
#include "Consumer.h"
#include "topic.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include <cstdint>

#define CAN_SYNC_ID      0x000
#define CAN_SYNC_MS      10000
//#define CAN_TX_GPIO      40
//#define CAN_RX_GPIO      39
#define CAN_TX_GPIO ((gpio_num_t)40)
#define CAN_RX_GPIO ((gpio_num_t)39)
#define CAN_STANDBY_GPIO 4

class CANTXConsumer : public Consumer {
private:
    twai_node_handle_t node_hdl          = nullptr;
    uint64_t           base_timestamp_us = 0;  //de x em x mensagens enviamos uma mensagem com um timestamp base com 8 bytes. Nas restantes mesnagens envia-se o delta, em vez do timstamp inteiro. 
    uint32_t           last_sync_ms      = 0;  

    void     sendSync();
    void     sendTopic(const topic_t1 &t);
    uint16_t scaleValue(uint8_t producer_id, float value);

public:
    CANTXConsumer() = default;

    void setup()                       override;
    void run()                         override;
    void consume(const topic_t1 &topic) override;
};