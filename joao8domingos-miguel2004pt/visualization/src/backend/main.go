package main

import (
	"context"
	"encoding/binary"
	"fmt"
	"log"
	"os"
	"os/signal"
	"syscall"
	"time"
	"visualization-stack/schema"

	influxdb3 "github.com/InfluxCommunity/influxdb3-go/influxdb3"
	mqtt "github.com/eclipse/paho.mqtt.golang"
)

const (
	broker   = "tcp://mosquitto:1883"
	topic    = "psem/telemetry/stream"
	clientID = "backend-subscriber"
)

// Nomes legíveis para cada sensor ID
// main.go — atualizar sensorNames
var sensorNames = map[uint8]string{
	schema.SensorVoltage:    "Voltage (V)",
	schema.SensorCurrent:    "Current (A)",
	schema.SensorTemp:       "Temperature (°C)",
	schema.SensorSpeed:      "Speed (km/h)",
	schema.SensorSteering:   "Steering Angle (°)",
	schema.SensorCPUUsage:   "CPU Usage (%)",
	schema.SensorQueueSize:  "Queue Size",
	schema.SensorTickHealth: "Tick Health (ms)",
	schema.SensorCPUCore1:   "CPU Core 1 (%)",
}

func isMetric(sensorID uint8) bool {
	return sensorID == schema.SensorCPUUsage ||
		sensorID == schema.SensorCPUCore1 ||
		sensorID == schema.SensorQueueSize ||
		sensorID == schema.SensorTickHealth
}

func decodePayload(data []byte) ([]*schema.SensorPayload, error) {
	if len(data) < 8 {
		return nil, fmt.Errorf("payload demasiado curto: %d bytes", len(data))
	}

	baseTimestampUs := binary.LittleEndian.Uint64(data[0:8])

	remaining := data[8:]
	var payloads []*schema.SensorPayload

	for i := 0; i < len(remaining); {
		// Tamanho mínimo: 5 bytes (id + delta + value)
		if i+5 > len(remaining) {
			return nil, fmt.Errorf("payload truncado no offset %d", i)
		}

		sensorID := remaining[i]
		deltaUs := binary.LittleEndian.Uint16(remaining[i+1 : i+3])
		valueScaled := binary.LittleEndian.Uint16(remaining[i+3 : i+5])

		var deviceID uint16
		fieldSize := 5

		if isMetric(sensorID) {
			if i+7 > len(remaining) {
				return nil, fmt.Errorf("payload truncado para métrica 0x%02X no offset %d", sensorID, i)
			}
			deviceID = binary.LittleEndian.Uint16(remaining[i+5 : i+7])
			fieldSize = 7
		}

		i += fieldSize

		timestampUs := baseTimestampUs + uint64(deltaUs)
		timestampS := uint32(timestampUs / 1_000_000)

		payloads = append(payloads, &schema.SensorPayload{
			SensorID:  sensorID,
			DeviceID:  deviceID,
			Value:     unscaleValue(sensorID, valueScaled),
			Timestamp: timestampS,
		})
	}

	return payloads, nil
}

func unscaleValue(sensorID uint8, scaled uint16) float32 {
	switch sensorID {
	case 0x01: // Voltage
		return float32(scaled) / 100.0
	case 0x02: // Current
		return float32(scaled) / 100.0
	case 0x03: // Temperature
		return float32(scaled)/10.0 - 40.0
	case 0x04: // Speed
		return float32(scaled) / 100.0
	case 0x05: // Steering
		return float32(scaled)/100.0 - 180.0
	default:
		return float32(scaled)
	}
}

func onMessage(_ mqtt.Client, msg mqtt.Message) {
	payloads, err := decodePayload(msg.Payload())
	if err != nil {
		log.Printf("[ERROR] %v\n", err)
		return
	}

	for _, payload := range payloads {
		name, ok := sensorNames[payload.SensorID]
		if !ok {
			name = fmt.Sprintf("Sensor 0x%02X", payload.SensorID)
		}
		t := time.Unix(int64(payload.Timestamp), 0).Format("15:04:05")
		log.Printf("[%s] %s -> %.2f\n", t, name, payload.Value)
	}
}

// makeMessageHandler retorna um mqtt.MessageHandler que processa mensagens MQTT e escreve os dados no InfluxDB
func makeMessageHandler(influx *influxdb3.Client) mqtt.MessageHandler {
	return func(_ mqtt.Client, msg mqtt.Message) {
		payloads, err := decodePayload(msg.Payload()) // Decodifica o payload MQTT usando a função decodePayload
		if err != nil {
			log.Printf("[ERROR] %v\n", err)
			return
		}

		for _, payload := range payloads { // Para cada payload decodificado, processa os dados
			name, ok := sensorNames[payload.SensorID]
			if !ok {
				name = fmt.Sprintf("Sensor 0x%02X", payload.SensorID)
			}

			t := time.Unix(int64(payload.Timestamp), 0) // Imprime os dados do sensor na consola para depuração
			log.Printf("[%d][%s] %s -> %.2f\n", payload.Timestamp, t.Format("15:04:05"), name, payload.Value)

			point := influxdb3.NewPoint("sensor_data1",
				map[string]string{
					"sensor_id":   fmt.Sprintf("%d", payload.SensorID),
					"sensor_name": name,
					"device_id":   fmt.Sprintf("0x%04X", payload.DeviceID), // útil para métricas
				},
				map[string]interface{}{
					"value": payload.Value,
				},
				t,
			)

			if err := influx.WritePoints(context.Background(), []*influxdb3.Point{point}); err != nil {
				log.Printf("[ERROR] InfluxDB write failed: %v\n", err)
			}
		}
	}
}

func main() {
	// Configura o cliente InfluxDB
	influx, err := influxdb3.New(influxdb3.ClientConfig{
		Host:     "http://influxdb3-core:8181",
		Token:    "apiv3_61spKjNfyc6n6kZm1a8-XSr37YWxYFNfuX3k-mSFmI5L08JyPRw1CD2HGDKrtZNW985g9pd_w1OMyxfbEJhh2g",
		Database: "telemetry",
	})

	if err != nil {
		log.Fatalf("Failed to create InfluxDB client: %v", err)
	}

	defer influx.Close() // Certifique-se de fechar o cliente InfluxDB ao finalizar

	opts := mqtt.NewClientOptions().AddBroker(broker).SetClientID(clientID).SetUsername("joao").SetPassword("2004") // Adicione autenticação MQTT

	client := mqtt.NewClient(opts) // Cria o cliente MQTT

	token := client.Connect() // Conecta ao broker MQTT
	token.Wait()              // Aguarda a conexão ser estabelecida

	if token.Error() != nil {
		log.Fatalf("Failed to connect to MQTT broker: %v", token.Error())
	}

	defer client.Disconnect(250) // Desconecta do broker MQTT ao finalizar

	token = client.Subscribe(topic, 1, makeMessageHandler(influx)) // Inscreve-se no tópico MQTT com o handler que escreve no InfluxDB
	token.Wait()                                                   // Aguarda a inscrição ser confirmada

	if token.Error() != nil {
		log.Fatalf("Failed to subscribe to topic '%s': %v", topic, token.Error())
	}

	log.Printf("Subscribed to topic '%s', waiting for messages...\n", topic)

	sigChan := make(chan os.Signal, 1)                    // Aguarda sinais de interrupção para encerrar o programa graciosamente
	signal.Notify(sigChan, os.Interrupt, syscall.SIGTERM) // Bloqueia até receber um sinal de interrupção
	<-sigChan                                             // Quando um sinal de interrupção for recebido, o programa continuará aqui e fará a limpeza necessária

	log.Println("Shutting down...")
}
