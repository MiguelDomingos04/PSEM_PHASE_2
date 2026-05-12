#pragma once
#include "Consumer.h"
#include "topic.h"
#include "freertos/event_groups.h"
#include "mqtt_client.h"
#include <cstdint>
#include <cstring>

class MQTTConsumer : public Consumer {
private:
    // WiFi
    EventGroupHandle_t wifi_event_group = nullptr;
    static constexpr int WIFI_CONNECTED_BIT = BIT0;

    // MQTT 
    esp_mqtt_client_handle_t mqtt_client = nullptr;

    // Formato otimizado por topic: [id: 1][delta_us: 2][value_scaled: 2] = 5 bytes
    static constexpr int FIELD_SIZE       = 1 + 2 + 2;
    static constexpr int BUFFER_THRESHOLD = 10;
    //static constexpr int BUFFER_SIZE      = BUFFER_THRESHOLD * FIELD_SIZE;
    static constexpr int BUFFER_SIZE = sizeof(uint64_t) + BUFFER_THRESHOLD * FIELD_SIZE; // espaço extra para o timestamp base

    uint8_t  buffer[BUFFER_SIZE];
    uint32_t count = 0;

    // Timestamp base para calcular delta — igual ao CANTXConsumer
    uint64_t base_timestamp_us = 0;
    bool     base_set          = false; // indica se o timestamp base já foi definido (apenas no primeiro topic recebido)
    

    const char *ssid;
    const char *password;
    const char *broker_uri;
    const char *topic;

    void     wifiInit();
    void     mqttInit();
    void     packTopic(const topic_t& t);
    uint16_t scaleValue(uint8_t producer_id, float value);

    // Callbacks estáticos 
    static void wifiEventHandler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data);
    static void mqttEventHandler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data);

public:
    MQTTConsumer(const char *ssid, const char *password,
                 const char *broker_uri, const char *topic);

    void setup()                       override;
    void run()                         override;
    void consume(const topic_t& topic) override;
};