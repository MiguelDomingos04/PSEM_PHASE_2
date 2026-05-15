#pragma once
#include "topic.h"

// Classe base Consumer 
// Todos os consumers herdam desta classe e implementam:
//   setup()          → inicializar o hardware ou recursos necessários
//   run()            → loop do consumer (pode ser vazio se consume() for suficiente)
//   consume(topic_t) → processar um dado recebido da queue
// O TelemetryManager chama consume() sempre que há um novo dado na queue,
// para cada consumer registado.
//
class Consumer {
public:
    virtual ~Consumer() = default; // destrutor virtual para permitir herança polimórfica. Permite que o TelemetryManager delete um Consumer* sem saber a subclasse específica, garantindo que o destrutor correto seja chamado para libetar os recursos adequadamente.

    virtual void setup()                    = 0;  // inicializar hardware/recursos
    virtual void run()                      = 0;  // loop do consumer
    virtual void consume(const topic_t1 &topic) = 0;  // processar um dado
};