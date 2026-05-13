package schema

// schema.go — adicionar os novos IDs
const (
    SensorVoltage    uint8 = 0x01 // ID para o sensor de tensão
    SensorCurrent    uint8 = 0x02 // ID para o sensor de corrente
    SensorTemp       uint8 = 0x03 // ID para o sensor de temperatura
    SensorSpeed      uint8 = 0x04 // ID para o sensor de velocidade (RPM)
    SensorSteering   uint8 = 0x05 // ID para o sensor de direção (steering angle)
    SensorCPUUsage   uint8 = 0x06 // ID para o sensor de uso da CPU
    SensorQueueSize  uint8 = 0x07 // ID para o sensor de tamanho da fila de mensagens
    SensorTickHealth uint8 = 0x08 // ID para o sensor de saúde do tick (tick health)
)

type SensorPayload struct {
	SensorID uint8
	Value   float32
	Timestamp uint32
}
