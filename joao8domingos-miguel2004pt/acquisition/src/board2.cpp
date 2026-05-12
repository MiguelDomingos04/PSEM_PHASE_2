#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "TelemetryManager.h"
#include "producers/VoltageProducer.h"
#include "producers/CurrentProducer.h"
#include "producers/TempProducer.h"
#include "producers/SpeedProducer.h"
#include "producers/SteeringAngleProducer.h"
#include "producers/CANRXProducer.h"
#include "consumers/LCDConsumer.h"
#include "consumers/MQTTConsumer.h"
#include "consumers/CANTXConsumer.h"
#include "producers/PerformanceMetricsProducer.h"

#define WIFI_SSID      "HABIBI"
#define WIFI_PASSWORD  "hamud123"
#define BROKER_URI     "mqtt://10.37.104.45:1883"
#define MQTT_TOPIC     "psem/telemetry/stream"

extern "C" void app_main(void)
{
    // 6 producers (5 sensores + CANRXProducer), 3 consumers (LCD + MQTT + CANTXConsumer)
    static TelemetryManager<4, 3, 20> manager;

    // Producers locais + CANRXProducer que recebe dados do outro ESP32 via CAN
    static TempProducer          tempProducer(true);
    static SpeedProducer         speedProducer(true);
    static CANRXProducer         canRxProducer;
    static PerformanceMetricsProducer metricsProducer(5000);

    // Consumers — LCD, MQTT e CANTXConsumer que envia dados para o outro ESP32
    static LCDConsumer   lcdConsumer;
    static MQTTConsumer  mqttConsumer(WIFI_SSID, WIFI_PASSWORD, BROKER_URI, MQTT_TOPIC);
    static CANTXConsumer canTxConsumer;

    // Registar consumers ANTES dos producers
    manager.registerConsumers(&lcdConsumer, &mqttConsumer, &canTxConsumer);

    // Registar producers
    manager.registerProducers(&tempProducer, &speedProducer, &canRxProducer, &metricsProducer);

    manager.run();
}