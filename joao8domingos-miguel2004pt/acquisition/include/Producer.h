#pragma once
#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "topic.h"

//  Classe base Producer 
// Todos os producers herdam desta classe e implementam:
//   setup() → inicializar o hardware ou recursos necessários
//   run()   → loop de leitura — lê o sensor e envia para a queue
// O TelemetryManager chama setQueue() antes de arrancar o producer,
// para que ele saiba para onde enviar os dados.

class Producer {
protected:
    uint8_t      producerId; // ID do producer 
    QueueHandle_t destinationQueue = nullptr; // queue do TelemetryManager para enviar os topic_t

public:
    explicit Producer(uint8_t producerId) : producerId(producerId) {} // construtor para definir o producerId
    virtual ~Producer() = default; // destrutor virtual para permitir herança polimórfica. Permite que o TelemetryManager delete um Producer* sem saber a subclasse específica, garantindo que o destrutor correto seja chamado para libetar os recursos adequadamente.

    uint8_t getProducerId() const { return producerId; }

    void setQueue(QueueHandle_t queue) { destinationQueue = queue; }

    virtual void setup() = 0;  // inicializar hardware/recursos
    virtual void run()   = 0;  // loop de leitura e envio para a queue
};