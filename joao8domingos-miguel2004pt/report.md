# Relatório Técnico — Sistema de Telemetria PSEM
## Tarefa de Recrutamento Spring 2026 — Phase 2

**Autores:** João Domingos, Miguel Domingos

## 1. Visão Geral do Sistema

O sistema de telemetria desenvolvido é composto por três camadas independentes que comunicam entre si:

- **Phase 1 — Leitura de sensores individuais** com ADC contínuo, MCPWM e SPI
- **Phase 2 — Framework de aquisição** com arquitectura Producer-Consumer distribuída por dois (podem ser mais) ESP32 ligados por CAN bus, com streaming para dashboard LCD e broker MQTT
- **Phase 3 — Visualization stack** com backend Go, InfluxDB3 e Grafana, containerizada num Docker

O fluxo de dados do projeto é o seguinte (podendo ser adaptado de forma genérica, sendo possivel criar difernetes producers, consumers, ...):

```
[Dashboard ESP32]                        [Powerboard ESP32]
  VoltageProducer  ─┐                      TempProducer  ─┐
  CurrentProducer  ─┼→ CANTXConsumer ══CAN══> CANRXProducer ─┼→ MQTTConsumer → mosquitto → Go backend → InfluxDB → Grafana
                    ┘                      SpeedProducer ─┘         └→ LCDConsumer → Display ILI9341
```


## 2. Phase 1 — Leitura de Sensores

Todos os producers suportam um modo `useMockData = true` (activo por omissão) que gera valores com random walk em vez de ler do hardware. Isto permite testar o sistema completo sem sensores físicos ligados, o que se mostrou essencial para desenvolvimento e demonstração.
O codigo de setup de todos os producers foi baseado/copiado do da phase 1. 
De seguida, é indicado para cada variável física o sensor ou protocolo/interface utilizado:
Voltage: ADC continuous
Temperature: ADC continuous
Current: ADC continuous
SpeedProducer: MCPWM
Steering Angle: Amt22b com SPI 


## 3. Phase 2 — Acquisition Framework


**Prioridades FreeRTOS:** O dispatcher tem prioridade 6, producers e consumers têm prioridade 5. Isto garante que o dispatcher tem preferência sobre a produção de dados, evitando que a queue fique saturada em picos de actividade.

### Estrutura `topic_t1`

```cpp
struct topic_t1 {
    uint8_t  producer_id;   // 1 byte  — qual foi o sensor que gerou o dado/value
    uint64_t timestamp_us;  // 8 bytes — timestamp em µs 
    float    value;         // 4 bytes — valor físico (tensão, corrente, etc.)
    uint16_t device_id = 0; // 2 bytes — 0=sensor, !=0=métrica de sistema. 
};
```

**Razão do `device_id`:** As métricas de performance (CPU, queue size, tick health) são produzidas por ambos os ESP32 com os mesmos IDs de producer. Para distinguir a origem, foi adicionado um `device_id` derivado do MAC address onde se aplica o XOR que reduz o tamnaho do Mac address de 6 bytes para 2:

```cpp
uint16_t computeDeviceId() {
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    return ((uint16_t)mac[0]<<8|mac[1]) ^
           ((uint16_t)mac[2]<<8|mac[3]) ^
           ((uint16_t)mac[4]<<8|mac[5]);
}
```

O MAC address é único por dispositivo (gravado em eFuse de fábrica), pelo que o `device_id` também é único em principio (pode haver colisões de valores de device_id, apesar destas serem muito raras. Aumentar o numero de Esps no sistema aumenta a probabilidades de colisões). O valor `0` (impossível num MAC real após XOR) foi reservado para topics que guardam valores dos sensores, que não precisam da identificação da origem.

**Razão do nome `topic_t1`:** O nome `topic_t` entrava em conflito com definições internas do MQTT, causando erros de compilação. O sufixo `1` resolve o conflito sem alterar a semântica.


A comunicação CAN entre os dois ESP32 usa três tipos de frame:

**Frame de sincronização (Sync):**
```
CAN ID  = 0x000
Payload = [base_timestamp_us : 8 bytes]
```

**Frame de sensor (IDs 0x01–0x05):**
```
CAN ID  = producer_id
Payload = [delta_us : 2 bytes][value_scaled : 2 bytes]  →  4 bytes total
```

**Frame de métrica (IDs 0x06–0x09):**
```
CAN ID  = producer_id
Payload = [delta_us : 2 bytes][value_scaled : 2 bytes][device_id : 2 bytes]  →  6 bytes total
```

**Razão do delta em vez do timestamp completo:** Um timestamp absoluto em microsegundos ocupa 8 bytes. Com o protocolo de delta, o timestamp base é enviado apenas uma vez no frame de sync, e as mensagens subsequentes enviam apenas a diferença em relação a essa base, em 2 bytes (uint16_t, máximo 65 535 µs ≈ 65 ms). Quando o delta excede este limite, é enviado um novo sync e o delta é reiniciado a zero. Esta abordagem reduz o payload de cada frame de sensores de 8+4=12 bytes para apenas 6 bytes (no final serão 4 apenas no melhor dos casos). 

**Razão do `value_scaled`:** Os valores float perdem as casas decimais ao serem truncados para uint16_t. Para preservar a precisão, cada sensor tem um factor de escala fixo. Exemplos:

| Sensor | Escala | Exemplo |
|---|---|---|
| Tensão | ×100 | 25.2 V → 2520 |
| Corrente | ×100 | 7.5 A → 750 |
| Temperatura | (v+40)×10 | 33 °C → 730 (offset para valores negativos) |
| Velocidade | ×100 | 248.16 km/h → 24816 |
| Direcção | (v+180)×100 | -45° → 13500 (offset para negativos) |

O receptor aplica a operação inversa simétrica para recuperar o valor físico original.

Todos os sensores (hardware) possuem uma precisão limitada a duas casas decimais. Assim, é possível descartar todos os algarismos após a segunda casa decimal, uma vez que estes não representam valores reais nem relevantes, dado que o próprio hardware não possui esta precisão.


As mensagens MQTT usam um formato binário compacto com buffering para minimizar o tráfego na rede.

**Estrutura de um pacote MQTT:**
```
[base_timestamp_us : 8 bytes]
  [id:1][delta:2][value:2]           ← sensor,  5 bytes por entrada
  [id:1][delta:2][value:2][device:2] ← métrica, 7 bytes por entrada 
  ...  (até BUFFER_THRESHOLD = 10 entradas) -> dentro do buffer podemos ter pacotes de 5 ou 7 bytes, dependendo se são metricas performance ou      valores medidos por sensores
```

**Razão do buffering com threshold de 10:** Publicar uma mensagem MQTT por cada leitura de sensor geraria um elevado numero de pacotes enviados. Acumular 10 leituras por pacote reduz este número por um factor de 10, diminuindo significativamente o overhead de protocolo MQTT (headers, QoS handshakes) e a carga no broker.

**Razão do flush por overflow de delta:** Se o intervalo entre duas leituras consecutivas no mesmo buffer exceder 65 ms, o campo `delta_us` em 2 bytes não consegue representar a diferença. Neste caso, o buffer parcial é publicado imediatamente antes que o overflow ocorra, e uma nova base é estabelecida. Isto garante que todos os deltas dentro de um pacote são sempre representáveis em 2 bytes.

**Razão do formato binário vs JSON:** JSON seria legível por humanos mas ocupa 5–10× mais bytes por mensagem. Para um sistema de telemetria com múltiplos sensores a 100 ms ou menos de período de amostragem, o JSON aumentaria significativamente a latência e a carga no broker. O formato binário tem uma estrutura fixa e conhecida, decodificável eficientemente pelo backend Go.

**Razão do QoS 1:** QoS 1 garante que cada mensagem é entregue pelo menos uma vez ao broker. QoS 0 (fire-and-forget) poderia perder leituras críticas em condições de rede instável. QoS 2 (exactly-once) adicionaria latência desnecessária para telemetria onde uma leitura em duplicado é preferível a uma leitura perdida.


## 4. Phase 3 — Métricas de Performance

O `PerformanceMetricsProducer` é um producer especial que monitoriza o estado interno do sistema em vez de ler hardware externo. Produz quatro métricas a cada 5 segundos (configurável), com IDs próprios no intervalo 0x06–0x09.

**Métricas produzidas:**

| ID | Métrica | Descrição |
|---|---|---|
| 0x06 | CPU Core 0 | Percentagem de utilização do núcleo 0 |
| 0x07 | Queue Size | Número de `topic_t1` pendentes na queue central |
| 0x08 | Tick Health | Desvio em ms face ao período de amostragem esperado |
| 0x09 | CPU Core 1 | Percentagem de utilização do núcleo 1 |

**Estimativa de uso de CPU por núcleo:** O ESP32-S3 é dual-core.  Medir apenas um núcleo daria uma visão incompleta da carga do sistema.

A estimativa baseia-se no tempo que a task Idle de cada núcleo passou a executar. Quanto mais tempo o CPU estiver ocupado com tasks reais, menos tempo a task Idle executa:

```cpp
float idle_pct = ((float)delta_idle / (float)delta_total) * 100.0f;
float cpu_pct  = 100.0f - idle_pct;
```

A métrica é calculada com deltas entre amostras consecutivas (e não médias acumuladas desde o boot), de forma a reflectir o uso de CPU no último intervalo de amostragem e manter-se representativa do estado atual. 

**Queue Size:** A queue central do TelemetryManager tem capacidade uma finita que é defenida ao inicializar o TelemetryManager. Se os producers gerarem dados mais rapidamente do que os consumers os processam, a queue enche e as leituras começam a ser descartadas. Monitorizar o tamanho da queue em tempo real permite detectar este problema de backpressure e torná-lo visível no Grafana.

**Tick Health:** O `vTaskDelay(pdMS_TO_TICKS(sample_ms))` não garante precisão absoluta — o FreeRTOS pode atrasar o acordar de uma task se o scheduler estiver ocupado. A Tick Health mede o desvio real entre o período esperado e o período efectivo:

```cpp
TickType_t elapsed_ms   = (now - last_wake_tick) * portTICK_PERIOD_MS;
float      deviation_ms = (float)elapsed_ms - (float)sample_ms;
return deviation_ms > 0.0f ? deviation_ms : 0.0f;
```

Desvios elevados indicam que o sistema está sobrecarregado ou que uma task de alta prioridade está a monopolizar o CPU, afectando os períodos de amostragem dos sensores.

**Razão de incluir `device_id` apenas nas métricas:** As métricas de CPU e queue são produzidas por ambos os ESP32 com os mesmos IDs (0x06–0x09). Sem identificação de origem, seria impossível distinguir no Grafana se um pico de CPU ocorreu no Dashboard ou no Powerboard. Os sensores físicos (0x01–0x05) não precisam de `device_id` porque em principio existirá apenas um sensor para cada variavel ficisca no sistema final. A macro `TOPIC_IS_METRIC(id)` é usada no `CANTXConsumer` e no `MQTTConsumer` para decidir se os 2 bytes extra de `device_id` são serializados no payload, mantendo os frames dos sensores com o tamanho mínimo (4 bytes CAN, 5 bytes MQTT).


## 5. Phase 4 — Visualization Stack

A visualization stack é containerizada num Docker Compose com serviços independentes:

```
ESP32 ──Wi-Fi──→ mosquitto (MQTT broker)
                        ↓
                  Go backend (subscriber + decoder)
                        ↓
              InfluxDB (time-series database)
                        ↓
                   Grafana (dashboards em tempo real)
```

### 5.1 Refatorização do Backend

O backend em Go foi atualizado para suportar o novo formato binário compacto
introduzido na Phase 3. As principais alterações foram:

**Formato do payload MQTT**

O payload deixou de ter campos de tamanho fixo e passou a ter tamanho variável
por entrada, dependendo do tipo de dado:

| Tipo | Formato | Tamanho |
|------|---------|---------|
| Sensor físico | `[id:1][delta_us:2][value_scaled:2]` | 5 bytes |
| Métrica de sistema | `[id:1][delta_us:2][value_scaled:2][device_id:2]` | 7 bytes |

O cabeçalho de cada pacote contém sempre um `base_timestamp_us` de 8 bytes,
a partir do qual os timestamps individuais são reconstruídos via delta.

**Função `decodePayload`**

A função de deserialização foi reescrita para lidar com inputs de tamanho
variável. Em vez de iterar com um offset fixo de 5 bytes, lê o `sensor_id`
de cada entrada e determina o tamanho do campo dinamicamente:

```go
func isMetric(sensorID uint8) bool {
    return sensorID == schema.SensorCPUUsage ||
        sensorID == schema.SensorCPUCore1   ||
        sensorID == schema.SensorQueueSize  ||
        sensorID == schema.SensorTickHealth
}
```

Para as métricas, são lidos 7 bytes em vez de 5, extraindo também o `device_id`
que identifica qual ESP32 gerou a métrica — necessário porque ambas as boards
publicam métricas de sistema no mesmo tópico MQTT.

**Schema**

O `schema.go` foi atualizado com dois novos campos:
- `SensorCPUCore1 = 0x09` — monitorização do segundo core do ESP32-S3
- `DeviceID uint16` na struct `SensorPayload` — identificador da board de origem

**Escrita no InfluxDB**

O `device_id` é agora escrito em todos os tópicos recebidos,
permitindo filtrar por board nas queries:

```go
map[string]string{
    "sensor_id":   fmt.Sprintf("%d", payload.SensorID),
    "sensor_name": name,
    "device_id":   fmt.Sprintf("0x%04X", payload.DeviceID),
}
```

### 5.2 Expansão do Dashboard Grafana

O dashboard foi expandido para incluir as novas métricas de saúde do sistema
introduzidas pelo `PerformanceMetricsProducer`.

**Variável `device_id`**

Foi criada uma variável de template do tipo Custom com os `device_id` das duas
boards. Isto permite selecionar
no dropdown do dashboard qual ESP32 se está a monitorizar, sem duplicar panels.

**Separação de queries por tipo de dado**

As queries do dashboard foram divididas em dois grupos:

- Sensores físicos (`sensor_id` 1–5: tensão, corrente, temperatura, velocidade,
  ângulo de direção) — filtram apenas por `sensor_id`, sem `device_id`, porque
  cada sensor existe exclusivamente numa das boards.
- Métricas de sistema (`sensor_id` 6–9: CPU Core 0, CPU Core 1, Queue Size,
  Tick Health) — filtram por `sensor_id` **e** `${device_id}`, porque ambas
  as boards publicam estas métricas.

Criou-se novas visualizations para as métricas do sistema (tanto time series como Gauge/Stat).
No caso da métrica da medição do uso do CPU, para as time series apresenta-se na mesma visualization o consumo de ambos os cores.

**Novas panels**

| Panel | Query | Tipo |
|-------|-------|------|
| CPU Usage (%) | `sensor_id IN ('6','9')` + `device_id` | Time series (2 linhas) |
| Queue Size | `sensor_id = '7'` + `device_id` | Stat |
| Tick Health (ms) | `sensor_id = '8'` + `device_id` | Stat |

O intervalo de tempo das queries de histórico usa `now() - interval '10 minutes'`
para manter o gráfico relevante, enquanto as panels de valor instantâneo usam
`ORDER BY time DESC LIMIT 1`.