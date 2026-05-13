# Data Visualization Report - João Domingos

## Phase 1: Deploy do Broker MQTT

A primeira fase consistiu na configuração do Eclipse Mosquitto como broker MQTT, adicionando-o ao `docker-compose.yml` com as portas `1883` (MQTT TCP) e `9001` (WebSocket) expostas (coloquei a porta 9001 apesar de nao ser usada). Criei um ficheiro `mosquitto.conf` mínimo com persistência de dados e logging ativados, montado no container como volume.

Foi implementada autenticação obrigatória com `allow_anonymous false`, usando o `mosquitto_passwd` para gerar um ficheiro de passwords encriptado.

**Dificuldade encontrada:** Após o primeiro `docker compose up`, o broker arrancava mas os logs mostravam um erro ao carregar o ficheiro de passwords. Investigando com `docker compose logs mosquitto`, o problema era as permissões do ficheiro `passwd` — o Mosquitto recusa carregar ficheiros de passwords que sejam legíveis publicamente, por razões de segurança. Para resolver este problema, restringi as permissões do ficheiro com `chmod 700 mosquitto/passwd`, garantindo que só o dono tem acesso. Após esta correção o broker arrancou corretamente.

A validação foi feita com duas sessões `docker exec`, uma a correr `mosquitto_sub` e outra a correr `mosquitto_pub`, confirmando o correto routing das mensagens.

Ao longo da minha vida académica tinha utilizado poucas vezes dockers, tendo aproveitado então para aprender mais através de tutoriais, de forma a conseguir implementar a 1º fase e as restantes.


## Phase 2: Backend e Gerador de Dados

### Schema do Payload

Para o formato dos payloads MQTT escolhi um esquema binário customizado (byte-packed), em detrimento das alternativas como JSON ou Protobuf. Esta decisão foi tomada conscientemente desde o início por várias razões: o formato binário é significativamente mais compacto (9 bytes fixos por mensagem vs ~40 bytes em JSON), determinístico, e mais próximo do que os sistemas embebidos reais utilizam em protocolos de telemetria como CAN bus.

O Protobuf foi descartado por introduzir complexidade desnecessária para este caso, pois requer um compilador de schemas e ficheiros `.proto`, e a vantagem principal que apresenta (geração automática de código para múltiplas linguagens) não se justifica neste caso pelo motivo de apenas utilizar Go e um schema simples de 5 sensores. O formato binário oferece eficiência equivalente sem dependências externas.

Cada payload tem a seguinte estrutura:

| Bytes | Campo | Tipo |
|-------|-------|------|
| 0 | `sensor_id` | `uint8` |
| 1–4 | `value` | `float32` big-endian |
| 5–8 | `timestamp` | `uint32` big-endian |

Mapeamento do id para o tipo de dados: `0x01` (Voltage), `0x02` (Motor Temperature), `0x03` (Current), `0x04` (RPM) e `0x05` (Battery SoC).

### Backend

O backend e o generator foram escritos em Go. A escolha foi motivada por duas razões: por um lado, o enunciado recomendava explicitamente a linguagem; por outro, era uma oportunidade de aprofundar o conhecimento numa linguagem com a qual tinha pouca experiência. O Go tem suporte oficial para as tecnologias usadas nesta stack (a biblioteca `paho.mqtt.golang` para comunicação com o Mosquitto e o `influxdb3-go` para escrita de dados no InfluxDB 3 Core). Isto garantiu uma integração direta e bem suportada com os restantes serviços, sem necessidade de soluções alternativas.

O backend subscreve o tópico `telemetry/sensors` no broker Mosquitto. Para cada mensagem recebida, descodifica o payload binário e regista os valores no terminal (antes de ter contanereizado).

### Generator de Dados Mock

O gerador publica leituras de todos os sensores a cada 100ms. Por iniciativa própria, os valores gerados seguem modelos físicos realistas em vez de serem puramente aleatórios:

- **Voltage**: oscilação sinusoidal em torno de 14V, simulando o comportamento de um alternador
- **Motor Temperature**: equação diferencial `dT/dt = heatGen - cooling * (T - T_air)`, onde a temperatura acumula ao longo do tempo com aquecimento por throttle e arrefecimento proporcional
- **RPM**: oscilação sinusoidal lenta em torno de 3000 RPM
- **Current**: proporcional ao RPM, simulando o consumo real do motor
- **Battery SoC**: sobe quando a tensão está alta (alternador a carregar), desce caso contrário

Os sensores com dependência temporal (temperatura, SoC) mantêm o estado entre ticks através de variáveis globais persistentes.

### Containerização

O backend foi containerizado usando um Dockerfile multi-stage: a primeira fase usa `golang:1.26-alpine` para compilar o binário, e a segunda fase usa `alpine:latest` com apenas o binário compilado, resultando numa imagem final de ~10MB em vez de ~300MB.

Foi também criado um `Dockerfile_GEN` para o gerador, deixando a possibilidade de o containerizar no futuro. Acabei por comentar no docker-compose.yml a containerização do generator, por não ser necessária, e por vezes incomodativa.


## Phase 3: Base de Dados InfluxDB

### Deploy e Configuração

Foi adicionado o InfluxDB 3 Core ao `docker-compose.yml`. Para autenticação, foi gerado um token offline:

```bash
docker run --rm \
  -v $(pwd):/output \
  quay.io/influxdb/influxdb3-core:latest \
  create token --admin --offline --output-file /output/admin-token.json
```

A decisão de gerar o token offline foi pelo facto de um token gerado online ser guardado na base de dados interna do InfluxDB e desaparece quando o container e os volumes são destruídos, obrigando a criar um novo token a cada reset. Com o token offline, o ficheiro `admin-token.json` é montado no container através do volume e o servidor reconhece-o sempre ao arrancar através do argumento `--admin-token-file`, tornando a stack completamente reproduzível.

O InfluxDB Explorer foi também adicionado como serviço separado. Foi criado um `config.json` para pré-configurar a ligação ao Core, evitando que o utilizador tenha de introduzir manualmente o endereço do servidor e o token ao abrir o browser.

### Dificuldades e Soluções

**Problema:** A imagem usada inicialmente para o Explorer (`quay.io/influxdb/influxdb3-explorer`) não funcionava corretamente. A imagem correta é a `influxdata/influxdb3-ui`, que expõe a porta `80` (o mapeamento correto é `8888:80` e não `8888:8888`).

**Problema:** O Explorer mostrava um problema ao abrir o browser. O problema era que a `SESSION_SECRET_KEY` definida no `.env` tinha menos de 32 caracteres. O Explorer rejeita chaves demasiado curtas para assinar as cookies da sessão. Aumentei a chave para 32 caracteres e resolveu-se o problema.

### Integração no Backend

O backend foi atualizado para escrever os dados no InfluxDB. A função de callback MQTT tem uma assinatura fixa e não aceita argumentos extra, pelo que foi necessário criar a função `makeMessageHandler`, uma função que recebe o cliente InfluxDB como argumento e devolve o callback, usando um closure para manter o acesso ao cliente dentro do handler.

Os dados são guardados numa única measurement `sensor_data`, com tags `sensor_id` e `sensor_name` para identificar o sensor, e o campo `value` para o valor da leitura. Esta estrutura foi escolhida de forma a centralizar todas as leituras numa única tabela, simplificando a gestão e permitindo filtrar por sensor nas queries com `WHERE sensor_id = '1'`.


## Phase 4: Visualização com Grafana

### Deploy e Configuração

Adicionei o Grafana ao `docker-compose.yml` com um volume para persistência dos dashboards e a variável de ambiente `GF_SECURITY_ADMIN_PASSWORD` para definir a password do utilizador admin. O InfluxDB foi configurado como data source usando SQL como query language.

### Dashboard

O dashboard *Telemetry* foi construído com dois tipos de painéis para cada sensor, com o objetivo de apresentar simultaneamente o valor imediato e a evolução temporal dos dados:

- **Gauge / Stat** — mostram o valor mais recente de cada sensor em tempo real
- **Time series** — mostram a evolução temporal dos últimos 5 minutos

As queries usadas seguem dois padrões distintos consoante o tipo de painel:

```sql
-- Valor atual (Gauge/Stat)
SELECT value FROM sensor_data
WHERE sensor_id = '1'
ORDER BY time DESC
LIMIT 1

-- Evolução temporal (Time series)
SELECT time, value FROM sensor_data
WHERE sensor_id = '1'
AND time >= now() - interval '10 minutes'
ORDER BY time
```

O `ORDER BY time DESC LIMIT 1` garante que o Gauge mostra sempre a leitura mais recente, independentemente do time range selecionado no dashboard. Os time series usam um intervalo fixo de 10 minutos na query para garantir que há sempre dados visíveis.

### Atualização em Tempo Real

A maior dificuldade desta fase foi conseguir que o dashboard atualizasse a cada 500ms. O Grafana tem por defeito um intervalo mínimo de refresh de 5 segundos, e a interface não permitia guardar valores abaixo desse limite, sempre que alterava o Json Model e guardava as novas configurações, o timepicker voltava às configurações default. A solução passou por forçar no url o refresh de 500ms. O Grafana interpreta estes parâmetros no URL e aplica-os independentemente das configurações guardadas no dashboard, contornando as limitações da interface gráfica.