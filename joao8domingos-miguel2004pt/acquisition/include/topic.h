#pragma once
#include <cstdint>

// IDs dos producers (1 byte cada)
#define PRODUCER_ID_VOLTAGE  0x01
#define PRODUCER_ID_CURRENT  0x02
#define PRODUCER_ID_TEMP     0x03
#define PRODUCER_ID_SPEED    0x04
#define PRODUCER_ID_STEERING  0x05 
#define PRODUCER_ID_CPU_CORE_0  0x06
#define PRODUCER_ID_QUEUE_SIZE  0x07
#define PRODUCER_ID_TICK_HEALTH 0x08
#define PRODUCER_ID_CPU_CORE_1  0x09

// Verdadeiro para qualquer producer_id que seja métrica
#define TOPIC_IS_METRIC(id) ((id) == PRODUCER_ID_CPU_CORE_0 || \
                             (id) == PRODUCER_ID_CPU_CORE_1 || \
                             (id) == PRODUCER_ID_QUEUE_SIZE  || \
                             (id) == PRODUCER_ID_TICK_HEALTH)


// Estrutura que viaja na queue entre producers e consumers
// producer_id  → identifica quem gerou o dado (qual sensor)
// timestamp_us → quando foi gerado, em microssegundos desde o boot
// value        → o valor do campo (tensão, corrente, temperatura, etc.)
//
struct topic_t {
    uint8_t  producer_id;   // 1 byte  — ID do producer
    uint64_t timestamp_us;  // 8 bytes — timestamp em microssegundos
    float    value;         // 4 bytes — valor do campo
    uint16_t device_id = 0;  // 2 bytes — 0 = sensor, != 0 = métrica
};