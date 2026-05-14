#include "consumers/CANTXConsumer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include <cstring>
#include "time_of_day.h"

#define TAG "CANTXConsumer"

//  setup 
void CANTXConsumer::setup()
{
    gpio_reset_pin((gpio_num_t)CAN_STANDBY_GPIO); // GPIO para tirar o transceiver do modo standby
    gpio_set_direction((gpio_num_t)CAN_STANDBY_GPIO, GPIO_MODE_OUTPUT); // Configura o pino como saída. Isto é necessário para controlar o estado do transceiver CAN, permitindo que o microcontrolador ative ou desative a comunicação CAN conforme necessário. Definir o pino como saída garante que o microcontrolador possa enviar um sinal para o transceiver, controlando seu modo de operação (standby ou ativo) e garantindo uma comunicação eficiente e confiável na rede CAN.
    gpio_set_level((gpio_num_t)CAN_STANDBY_GPIO, 0); // Define o nível baixo para tirar o transceiver do modo standby

    twai_onchip_node_config_t node_config = {}; 
    node_config.io_cfg.tx          = CAN_TX_GPIO; // Configura o GPIO para transmissão CAN, permitindo que o microcontrolador envie mensagens para a rede CAN. Este pino é essencial para a função de transmissão do CANTXConsumer, garantindo que os dados sejam enviados corretamente para os outros dispositivos na rede CAN.
    node_config.io_cfg.rx          = CAN_RX_GPIO; // Configura o GPIO para recepção CAN, permitindo que o microcontrolador receba mensagens da rede CAN. Este pino é essencial para a função de recepção do CANTXConsumer, garantindo que os dados sejam recebidos corretamente dos outros dispositivos na rede CAN, embora neste caso específico o foco seja na transmissão.   
    node_config.bit_timing.bitrate = 200000; // Configura a taxa de bits para a comunicação CAN, garantindo que o CANTXConsumer se comunique na velocidade correta com os outros dispositivos na rede CAN. A taxa de bits deve ser consistente com a configuração dos outros dispositivos para garantir uma comunicação eficiente e sem erros na rede CAN.
    node_config.tx_queue_depth     = 5; // Configura a profundidade da fila de transmissão, permitindo que o CANTXConsumer armazene várias mensagens para envio antes de bloqueá-lo. Esta configuração é importante para garantir que o sistema possa lidar com picos de mensagens a serem enviadas sem perder dados, proporcionando uma comunicação mais robusta e eficiente na rede CAN.

    /*
    twai_onchip_node_config_t node_config = {
        .io_cfg.tx          = CAN_TX_GPIO,
        .io_cfg.rx          = CAN_RX_GPIO,
        .bit_timing.bitrate = 200000,
        .tx_queue_depth     = 5,
    };
    */

    ESP_ERROR_CHECK(twai_new_node_onchip(&node_config, &node_hdl));
    ESP_ERROR_CHECK(twai_node_enable(node_hdl));

    base_timestamp_us = (uint64_t)TimeReader::getCurrentTimeUs();
    sendSync();

    ESP_LOGI(TAG, "CANTXConsumer iniciado — TX=GPIO%d RX=GPIO%d",
             CAN_TX_GPIO, CAN_RX_GPIO);
}

// run 
// Verifica se é necessário enviar sync
void CANTXConsumer::run()
{
    //envia de 10 em 10 segundos um sync para manter o timestamp base atualizado, mesmo que não haja mensagens a enviar. Isto é importante para garantir que os deltas de tempo das mensagens subsequentes não se tornem muito grandes, o que poderia levar a erros de sincronização no receptor. O envio regular de syncs mantém a linha do tempo dos eventos precisa e permite que o sistema se recupere rapidamente de quaisquer interrupções ou atrasos na comunicação.
    uint32_t now_ms = (uint32_t)(TimeReader::getCurrentTimeUs() / 1000);
    if ((now_ms - last_sync_ms) >= CAN_SYNC_MS) {
        sendSync();
    }
    vTaskDelay(pdMS_TO_TICKS(10)); 
}

// consume 
void CANTXConsumer::consume(const topic_t1 &t)
{
    sendTopic(t);
}

// sendSync 
// Envia timestamp base — CAN ID=0x000, payload=timestamp_us (8 bytes)
// Igual ao send_message() da Phase 1 mas com payload de sync
void CANTXConsumer::sendSync()
{
 
    last_sync_ms = (uint32_t)(TimeReader::getCurrentTimeUs() / 1000);

    uint8_t buf[8];
    memcpy(buf, &base_timestamp_us, sizeof(uint64_t));

    twai_frame_t tx_msg = {};

    tx_msg.header.id  = CAN_SYNC_ID;
    tx_msg.header.ide = false;
    tx_msg.buffer     = buf;
    tx_msg.buffer_len = sizeof(uint64_t);

    /*
    twai_frame_t tx_msg = {
        .header.id  = CAN_SYNC_ID,
        .header.ide = false,
        .buffer     = buf,
        .buffer_len = 8,
    };
    */
    ESP_ERROR_CHECK(twai_node_transmit(node_hdl, &tx_msg, 0));
    ESP_ERROR_CHECK(twai_node_transmit_wait_all_done(node_hdl, -1));

    ESP_LOGI(TAG, "Sync enviado — base_ts=%llu", base_timestamp_us);
}

/*
// sendTopic 
// Igual ao send_message() da Phase 1 mas com protocolo otimizado:
//   CAN ID  = producer_id (no header — não gasta payload)
//   Payload = [delta_us: 2][value_scaled: 2] = 4 bytes
void CANTXConsumer::sendTopic(consttopic_t1& t)
{
    
    
    if (t.timestamp_us < base_timestamp_us) {
        base_timestamp_us = t.timestamp_us; // Se o timestamp do topic for menor que o timestamp base, é necessário atualizar o timestamp base para evitar deltas negativos, garantindo que os deltas de tempo das mensagens subsequentes sejam calculados corretamente e mantendo a sincronização precisa dos dados no receptor. O envio de um sync após atualizar o timestamp base garante que o receptor esteja ciente da mudança e possa ajustar seus cálculos de tempo de acordo.
        sendSync();
    }

    uint64_t delta = t.timestamp_us - base_timestamp_us;

    //se o delta for maior que o que cabe em 2 bytes, é necessário enviar um sync para atualizar o timestamp base, garantindo que os deltas de tempo das mensagens subsequentes permaneçam dentro do limite de 2 bytes e evitando erros de sincronização no receptor. O envio de syncs regulares mantém a linha do tempo dos eventos precisa e permite que o sistema se recupere rapidamente de quaisquer interrupções ou atrasos na comunicação, garantindo uma comunicação eficiente e confiável na rede CAN.
    if (delta > 0xFFFF) {
        base_timestamp_us = t.timestamp_us; //define o novo timestamp base como sendo o timestamp do topic atual, garantindo que os deltas de tempo das mensagens subsequentes sejam calculados corretamente em relação ao novo timestamp base e mantendo a sincronização precisa dos dados no receptor.
        sendSync();
        delta = t.timestamp_us - base_timestamp_us;
    }

    uint16_t delta_us     = (uint16_t)delta;
    uint16_t value_scaled = scaleValue(t.producer_id, t.value);

    uint8_t buf[4];
    memcpy(buf,     &delta_us,     sizeof(uint16_t));
    memcpy(buf + 2, &value_scaled, sizeof(uint16_t));

    twai_frame_t tx_msg = {
        .header.id  = t.producer_id,
        .header.ide = false,
        .buffer     = buf,
        .buffer_len = 4,
    };
    ESP_ERROR_CHECK(twai_node_transmit(node_hdl, &tx_msg, 0));
    ESP_ERROR_CHECK(twai_node_transmit_wait_all_done(node_hdl, -1));

    ESP_LOGI(TAG, "CAN TX — id=0x%02X delta=%u val_scaled=%u",
             t.producer_id, delta_us, value_scaled);
}
*/

void CANTXConsumer::sendTopic(const topic_t1 &t)
{
    if (t.timestamp_us < base_timestamp_us) {
        base_timestamp_us = t.timestamp_us;
        sendSync();
    }

    uint64_t delta = t.timestamp_us - base_timestamp_us;
    if (delta > 0xFFFF) {
        base_timestamp_us = t.timestamp_us;
        sendSync();
        delta = 0;
    }

    uint16_t delta_us     = (uint16_t)delta;
    uint16_t value_scaled = scaleValue(t.producer_id, t.value);

    uint8_t buf[6];
    memcpy(buf,     &delta_us,     sizeof(uint16_t));
    memcpy(buf + 2, &value_scaled, sizeof(uint16_t));

    uint8_t payload_len;
    if (TOPIC_IS_METRIC(t.producer_id)) {
        memcpy(buf + 4, &t.device_id, sizeof(uint16_t));
        payload_len = 6;  // 4 + device_id
    } else {
        payload_len = 4;  // sem device_id
    }

    twai_frame_t tx_msg = {};
    tx_msg.header.id  = t.producer_id;
    tx_msg.header.ide = false;
    tx_msg.buffer     = buf;
    tx_msg.buffer_len = payload_len;

    /*
    twai_frame_t tx_msg = {
        .header.id  = t.producer_id,
        .header.ide = false,
        .buffer     = buf,
        .buffer_len = payload_len,
    };
    */
    ESP_ERROR_CHECK(twai_node_transmit(node_hdl, &tx_msg, 0));
    ESP_ERROR_CHECK(twai_node_transmit_wait_all_done(node_hdl, -1));

    ESP_LOGI(TAG, "CAN TX — id=0x%02X delta=%u val_scaled=%u len=%u",
             t.producer_id, delta_us, value_scaled, payload_len);
}







// scaleValue 
// Converte float → uint16_t com fator de escala por sensor.
// Necessário porque (uint16_t)25.2f = 25 — perdem-se as casas decimais.
// Com escala: 25.2 × 100 = 2520 → receiver: 2520 / 100.0f = 25.2 ✅
uint16_t CANTXConsumer::scaleValue(uint8_t producer_id, float value)
{
    switch (producer_id) {
        case PRODUCER_ID_VOLTAGE:
            return (uint16_t)(value * 100.0f);          // 18.0–25.2V → 1800–2520

        case PRODUCER_ID_CURRENT:
            return (uint16_t)(value * 100.0f);          // 0.0–30.0A → 0–3000

        case PRODUCER_ID_TEMP:
            return (uint16_t)((value + 40.0f) * 10.0f); // offset +40 para negativos

        case PRODUCER_ID_SPEED:
            return (uint16_t)(value * 100.0f);          // 0.0–300.0 km/h → 0–30000

        case PRODUCER_ID_STEERING:
            return (uint16_t)((value + 180.0f) * 100.0f); // offset +180 para negativos
        case PRODUCER_ID_CPU_CORE_0:
        case PRODUCER_ID_CPU_CORE_1:
            return (uint16_t)(value * 10.0f);     // 0–100% → 0–1000

        case PRODUCER_ID_QUEUE_SIZE:
            return (uint16_t)(value);             // já é inteiro

        case PRODUCER_ID_TICK_HEALTH:
            return (uint16_t)(value * 10.0f);    // ms → 0–10000

        default:
            return (uint16_t)value;
    }
}