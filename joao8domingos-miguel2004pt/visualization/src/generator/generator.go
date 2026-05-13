package main

import (
	"encoding/binary"
	"log"
	"math"
	"math/rand"
	"os"
	"os/signal"
	"syscall"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
	"visualization-stack/schema"
)

const (
	broker   = "tcp://localhost:1883" //mosquitto:1883
	topic    = "telemetry/sensors"
	clientID = "mock-generator"
	username = "joao"
	password = "2004"
	interval = 100 * time.Millisecond
)

type sensorRange struct {
	min, max float32
}


// Estado global dos sensores — persiste entre ticks
var state = struct {
    motorTemp   float64
    batterySoC  float64
    rpm         float64
}{
    motorTemp: 25.0,  
    batterySoC: 100.0,
    rpm: 0.0,
}

var w = 2 * math.Pi / 10.0 // frequência angular (10s)

func noise() float64 {
    return (rand.Float64() - 0.5) // ruído entre -0.5 e 0.5
}


func realisticValue(id uint8) float32 {
	t := float64(time.Now().UnixNano()) / 1e9
	dt := 0.1

	switch id {

	case schema.SensorVoltage:
		v := 14.0 + 2.0 * math.Sin(w * t) + noise() * 0.5

		return float32(v)

	case schema.SensorMotorTemp:
		throttle := 0.6
		T_air := 25.0
		heatGen := 80.0 * throttle
		cooling := 0.05

		dT := (heatGen - cooling * (state.motorTemp - T_air)) * dt

		state.motorTemp += dT + noise() * 0.5
		state.motorTemp = math.Max(T_air, math.Min(state.motorTemp, 120.0))

		return float32(state.motorTemp)

	case schema.SensorRPM:
		rpm := 3000.0 + 1500.0 * math.Sin(w * t * 0.3) + noise() * 100
		state.rpm = rpm

		return float32(state.rpm)

	case schema.SensorCurrent:
		current := (state.rpm / 8000.0) * 45.0 + noise() * 2

		return float32(math.Max(0, current))

	case schema.SensorBatterySoC:
		voltage := 14.0 + 1.5 * math.Sin(w * t)

		if voltage > 13.5 {
			state.batterySoC += 0.01
		} else {
			state.batterySoC -= 0.02
		}
		state.batterySoC = math.Max(0, math.Min(state.batterySoC, 100.0))

		return float32(state.batterySoC)

	default:
		return 0
	}
}



func encodePayload(sensorID uint8, value float32) []byte {
	buf := make([]byte, 9)

	buf[0] = sensorID

	bits := math.Float32bits(value)
	binary.BigEndian.PutUint32(buf[1:5], bits)

	binary.BigEndian.PutUint32(buf[5:9], uint32(time.Now().Unix()))

	return buf
}


func main() {
	opts := mqtt.NewClientOptions()
	opts.AddBroker(broker)
	opts.SetClientID(clientID)
	opts.SetUsername(username)
	opts.SetPassword(password)

	client := mqtt.NewClient(opts)

	token := client.Connect()
	token.Wait()

	if token.Error() != nil {
		log.Fatalf("Failed to connect to MQTT broker: %v", token.Error())
	}

	defer client.Disconnect(250)

	log.Printf("Connected to broker, publishing to '%s' every %s...\n", topic, interval)

	// Lista de sensores para iterar
	sensors := []uint8{
		schema.SensorVoltage,
		schema.SensorMotorTemp,
		schema.SensorCurrent,
		schema.SensorRPM,
		schema.SensorBatterySoC,
	}

	ticker := time.NewTicker(interval)
	defer ticker.Stop()

	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, os.Interrupt, syscall.SIGTERM)

	for {
		select {
		case <-ticker.C:
			// Publica uma leitura de cada sensor por tick
			for _, id := range sensors {
				value := realisticValue(id)
				payload := encodePayload(id, value)
				client.Publish(topic, 1, false, payload)
			}
		case <-sigChan:
			log.Println("Shutting down generator...")
			return
		}
	}
	
}