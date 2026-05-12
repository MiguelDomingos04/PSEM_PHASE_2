#include "MQTTConsumer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/inet.h"

#define TAG "MQTTConsumer"

MQTTConsumer::MQTTConsumer(const char *ssid, const char *password,
                            const char *broker_uri, const char *topic)
    : ssid(ssid), password(password), broker_uri(broker_uri), topic(topic)
{
    memset(buffer, 0, sizeof(buffer));
}

//  setup 
// Igual à Phase 1 — NVS, WiFi, MQTT
void MQTTConsumer::setup()
{
    esp_err_t ret = nvs_flash_init(); // Inicializa a partição NVS (Non-Volatile Storage) para armazenar dados persistentes, como configurações de rede ou logs de erro. A NVS é essencial para garantir que o dispositivo possa manter informações importantes mesmo após um reboot, permitindo uma operação mais robusta e confiável do sistema.
    // Se a partição NVS estiver cheia ou com uma versão incompatível, apagar e reinicializar
    /**
     * INICIALIZAÇÃO DA NVS (Non-Volatile Storage)
     * 
     * A NVS é essencial para armazenar dados que devem persistir após um reboot 
     * (como calibrações, configurações de rede ou logs de erro).
     * 
     * O fluxo abaixo tenta inicializar a partição padrão da Flash:
     * 1. Tenta a inicialização normal.
     * 2. Se a partição estiver cheia (NO_FREE_PAGES) ou se o formato dos dados 
     *    for de uma versão antiga/incompatível (NEW_VERSION_FOUND):
     *    - Força a limpeza total da partição (Erase).
     *    - Tenta inicializar novamente a partição vazia.
     * 
     * Nota: Em caso de erro crítico, o ESP_ERROR_CHECK provocará o reinício do sistema.
     */
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    wifiInit(); //inicialização do WiFi e conexão à rede
    mqttInit(); //inicialização do cliente MQTT e conexão ao broker

    ESP_LOGI(TAG, "MQTTConsumer iniciado — broker=%s topic=%s", broker_uri, topic);
}

// run 
// Igual à Phase 1 — orientado a eventos via consume()
void MQTTConsumer::run()
{
    vTaskDelay(pdMS_TO_TICKS(10));
}

//  consume 
// Igual à Phase 1 mas com formato otimizado — acumula no buffer e publica
// quando atinge o threshold
/*
void MQTTConsumer::consume(const topic_t& t)
{
    // Primeiro topic define o timestamp base
    if (!base_set) {
        base_timestamp_us = t.timestamp_us; // Define o timestamp base para o timestamp do primeiro topic recebido. Este timestamp base é usado para calcular os deltas de tempo para os tópicos subsequentes, permitindo que o sistema mantenha uma linha do tempo precisa dos eventos registrados pelos producers. Definir a base no primeiro tópico garante que os deltas sejam pequenos e precisos, melhorando a eficiência da codificação e a sincronização dos dados.
        base_set          = true;
    }

    // Se delta não cabe em 2 bytes (>65ms), resetar a base
    if (t.timestamp_us - base_timestamp_us > 0xFFFF) {
        base_timestamp_us = t.timestamp_us; // Resetar o timestamp base se o delta for maior que o limite de 2 bytes.
    }

    packTopic(t);
    count++;

    ESP_LOGI(TAG, "[%lu/%d] id=0x%02X delta=%llu val=%.2f",
             (unsigned long)count, BUFFER_THRESHOLD,
             t.producer_id,
             t.timestamp_us - base_timestamp_us,
             t.value);

    // Igual à Phase 1 — publica quando o buffer está cheio
    if (count >= BUFFER_THRESHOLD) {
        // Os primeiros 8 bytes do pacote a ser publicado é sempre o timstamp base.
        memcpy(buffer, &base_timestamp_us, sizeof(uint64_t)); // Preenche o início do buffer com o timestamp base, permitindo que o receptor reconstrua os timestamps completos dos tópicos subsequentes usando os deltas de tempo. Esta abordagem otimiza o uso do buffer, reduzindo a quantidade de dados enviados para cada tópico e mantendo a sincronização precisa dos eventos registrados pelos producers.

        int msg_id = esp_mqtt_client_publish(
            mqtt_client,
            topic,
            (const char *)buffer,
            BUFFER_SIZE,
            1,    // QoS 1 — igual à Phase 1
            0     // retain = false — igual à Phase 1
        );

        if (msg_id >= 0) {
            ESP_LOGI(TAG, "Buffer publicado — %d bytes → %s (msg_id=%d)",
                     BUFFER_SIZE, topic, msg_id);
        } else {
            ESP_LOGE(TAG, "Falha ao publicar — msg_id=%d", msg_id);
        }

        count    = 0;
        base_set = false;
        memset(buffer, 0, sizeof(buffer));
    }
}
*/

void MQTTConsumer::consume(const topic_t& t)
{
    if (!base_set) {
        base_timestamp_us = t.timestamp_us;
        base_set          = true;
    }

    //se o delta a meio ficar com um valor superior ao 0xFFFF, é necessaário enviar o pacote que se estava a criar e 
    if (t.timestamp_us - base_timestamp_us > 0xFFFF) {
        // Fazer flush do buffer antes de mudar a base
        if (count > 0) {
            memcpy(buffer, &base_timestamp_us, sizeof(uint64_t));
            int payload_size = (int)(sizeof(uint64_t) + buffer_offset);
            esp_mqtt_client_publish(mqtt_client, topic,
                                    (const char *)buffer, payload_size, 1, 0);
            count         = 0;
            buffer_offset = 0;
            memset(buffer, 0, sizeof(buffer));
        }
        base_timestamp_us = t.timestamp_us;
    }



    packTopic(t);
    count++;

    ESP_LOGI(TAG, "[%lu/%d] id=0x%02X delta=%llu val=%.2f device=0x%04X",
             (unsigned long)count, BUFFER_THRESHOLD,
             t.producer_id,
             t.timestamp_us - base_timestamp_us,
             t.value,
             t.device_id);

    if (count >= BUFFER_THRESHOLD) {
        memcpy(buffer, &base_timestamp_us, sizeof(uint64_t));

        // Tamanho real do payload = timestamp base + bytes acumulados
        int payload_size = (int)(sizeof(uint64_t) + buffer_offset);

        int msg_id = esp_mqtt_client_publish(
            mqtt_client, topic,
            (const char *)buffer, payload_size,
            1, 0
        );

        if (msg_id >= 0) {
            ESP_LOGI(TAG, "Buffer publicado — %d bytes → %s (msg_id=%d)",
                     payload_size, topic, msg_id);
        } else {
            ESP_LOGE(TAG, "Falha ao publicar — msg_id=%d", msg_id);
        }

        count         = 0;
        buffer_offset = 0;
        base_set      = false;
        memset(buffer, 0, sizeof(buffer));
    }
}



// Formato variável:
//   Sensor:  [id:1][delta:2][value:2]           — 5 bytes, device_id não enviado
//   Métrica: [id:1][delta:2][value:2][device:2] — 7 bytes, device_id enviado
void MQTTConsumer::packTopic(const topic_t& t)
{
    uint8_t *ptr = buffer + sizeof(uint64_t) + buffer_offset;

    uint16_t delta_us     = (uint16_t)(t.timestamp_us - base_timestamp_us);
    uint16_t value_scaled = scaleValue(t.producer_id, t.value);

    ptr[0] = t.producer_id;
    memcpy(ptr + 1, &delta_us,     sizeof(uint16_t));
    memcpy(ptr + 3, &value_scaled, sizeof(uint16_t));

    if (TOPIC_IS_METRIC(t.producer_id)) {
        memcpy(ptr + 5, &t.device_id, sizeof(uint16_t));
        buffer_offset += FIELD_SIZE_METRIC;  // +7
    } else {
        buffer_offset += FIELD_SIZE_SENSOR;  // +5
    }
}




//  packTopic 
// Formato otimizado (5 bytes por topic):
//   [0]    producer_id   uint8_t   1 byte
//   [1:2]  delta_us      uint16_t  2 bytes — diferença desde o timestamp base
//   [3:4]  value_scaled  uint16_t  2 bytes — value escalado por sensor
//
// Comparação com Phase 1:
//   Phase 1:  [id:1][timestamp_ms:4][value:4] = 9 bytes
//   Atual:    [id:1][delta_us:2][value:2]      = 5 bytes
/*
void MQTTConsumer::packTopic(const topic_t& t)
{
    
    uint8_t *ptr = buffer + sizeof(uint64_t) + (count * FIELD_SIZE); //coloca na zona certa do buffer (após o timestamp base e os tópicos anteriores) para guardar no buffer o novo topic
    uint16_t  delta_us     = (uint16_t)(t.timestamp_us - base_timestamp_us);
    uint16_t  value_scaled = scaleValue(t.producer_id, t.value);

    ptr[0] = t.producer_id;
    memcpy(ptr + 1, &delta_us,     sizeof(uint16_t));
    memcpy(ptr + 3, &value_scaled, sizeof(uint16_t));
}
*/

//  scaleValue 
// Igual ao CANTXConsumer — converte float para uint16_t preservando casas decimais
uint16_t MQTTConsumer::scaleValue(uint8_t producer_id, float value)
{
    switch (producer_id) {
        case PRODUCER_ID_VOLTAGE:
            return (uint16_t)(value * 100.0f);           // 18.0–25.2V → 1800–2520

        case PRODUCER_ID_CURRENT:
            return (uint16_t)(value * 100.0f);           // 0.0–30.0A → 0–3000

        case PRODUCER_ID_TEMP:
            return (uint16_t)((value + 40.0f) * 10.0f); // offset +40 para negativos

        case PRODUCER_ID_SPEED:
            return (uint16_t)(value * 100.0f);           // 0.0–300.0 km/h → 0–30000

        case PRODUCER_ID_STEERING:
            return (uint16_t)((value + 180.0f) * 100.0f); // offset +180 para negativos
        
        case PRODUCER_ID_CPU_CORE_0:
        case PRODUCER_ID_CPU_CORE_1:
            return (uint16_t)(value * 10.0f);    // 0–100% → 0–1000

        case PRODUCER_ID_QUEUE_SIZE:
            return (uint16_t)(value);            // já é inteiro

        case PRODUCER_ID_TICK_HEALTH:
            return (uint16_t)(value * 10.0f);   // ms → 0–10000

        default:
            return (uint16_t)value;
    }
}

//  wifiInit 
void MQTTConsumer::wifiInit()
{
    wifi_event_group = xEventGroupCreate(); // Cria um grupo de eventos para sincronizar a conexão WiFi. Este grupo é usado para sinalizar quando o dispositivo está conectado à rede WiFi, permitindo que outras partes do código aguardem essa condição antes de tentar usar a conexão de rede, garantindo uma operação mais robusta e evitando erros relacionados à falta de conectividade.

    ESP_ERROR_CHECK(esp_netif_init()); // Inicializa a pilha de rede TCP/IP do ESP-IDF, preparando o sistema para operações de rede. Esta função é essencial para configurar as interfaces de rede e garantir que o dispositivo possa se comunicar com outros dispositivos na rede, como o broker MQTT, permitindo a troca de dados e a funcionalidade de rede necessária para o MQTTConsumer operar corretamente.
    ESP_ERROR_CHECK(esp_event_loop_create_default()); // Cria um loop de eventos padrão para o sistema, permitindo que o ESP-IDF gere eventos de forma eficiente. Este loop é usado para processar eventos relacionados ao WiFi, MQTT e outros subsistemas, garantindo que as callbacks registradas sejam chamadas corretamente quando os eventos ocorrerem, como a conexão WiFi ou a obtenção de um endereço IP, facilitando a implementação de uma lógica orientada a eventos no MQTTConsumer.
    esp_netif_create_default_wifi_sta(); // Cria uma interface de rede padrão para o modo WiFi Station (STA), permitindo que o dispositivo se conecte a uma rede WiFi existente. Esta função é necessária para configurar a interface de rede que será usada para se conectar ao roteador WiFi, garantindo que o dispositivo possa acessar a rede e se comunicar com o broker MQTT, permitindo a funcionalidade principal do MQTTConsumer.

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT(); // Configuração padrão para inicialização do WiFi, fornecendo uma configuração básica que é adequada para a maioria dos casos de uso. Esta configuração inclui parâmetros como o número de buffers, o tamanho dos buffers e outras opções relacionadas ao driver WiFi, permitindo uma inicialização rápida e eficiente do subsistema WiFi sem a necessidade de personalizações complexas, facilitando o desenvolvimento do MQTTConsumer.
    ESP_ERROR_CHECK(esp_wifi_init(&cfg)); // Inicializa o driver WiFi com a configuração especificada, preparando o sistema para operações de WiFi. Esta função é essencial para configurar o hardware WiFi e garantir que o dispositivo esteja pronto para se conectar a redes WiFi, permitindo que o MQTTConsumer estabeleça uma conexão de rede necessária para se comunicar com o broker MQTT e realizar suas funções de consumo de dados.

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifiEventHandler, this)); // Registra um handler para eventos relacionados ao WiFi, permitindo que o MQTTConsumer responda a mudanças no estado da conexão WiFi, como conexões, desconexões e falhas. Este handler é essencial para implementar uma lógica de reconexão automática e para sinalizar quando o dispositivo está conectado à rede, garantindo que o MQTTConsumer possa operar de forma robusta mesmo em condições de rede instáveis.
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,wifiEventHandler, this)); // Registra um handler para eventos relacionados à obtenção de um endereço IP, permitindo que o MQTTConsumer responda quando o dispositivo obtém um endereço IP válido após se conectar a uma rede WiFi. Este handler é crucial para sinalizar que o dispositivo está pronto para se comunicar com outros dispositivos na rede, como o broker MQTT, garantindo que o MQTTConsumer possa iniciar suas operações de consumo de dados somente quando a conectividade de rede estiver estabelecida e funcional.

    wifi_config_t wifi_config = {}; // Estrutura de configuração para a conexão WiFi, que inclui os parâmetros necessários para se conectar a uma rede WiFi específica. Esta estrutura é preenchida com o SSID e a senha fornecidos ao criar o MQTTConsumer, permitindo que o dispositivo se autentique e se conecte à rede WiFi correta, garantindo que o MQTTConsumer possa acessar a rede e se comunicar com o broker MQTT para realizar suas funções de consumo de dados.
    strncpy((char *)wifi_config.sta.ssid,     ssid,     sizeof(wifi_config.sta.ssid)); // Copia o SSID da rede WiFi para a estrutura de configuração, garantindo que o dispositivo saiba a qual rede se conectar. O uso de strncpy garante que a string seja copiada de forma segura, evitando estouros de buffer e garantindo que o SSID seja corretamente configurado para a conexão WiFi, permitindo que o MQTTConsumer estabeleça uma conexão de rede bem-sucedida.
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password)); // Copia a senha da rede WiFi para a estrutura de configuração, permitindo que o dispositivo se autentique corretamente ao tentar se conectar à rede. Assim como com o SSID, o uso de strncpy garante uma cópia segura da string, evitando problemas de segurança e garantindo que a senha seja configurada corretamente para a conexão WiFi, permitindo que o MQTTConsumer estabeleça uma conexão de rede segura e funcional.

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA)); // Configura o modo de operação do WiFi para Station (STA), permitindo que o dispositivo se conecte a uma rede WiFi existente. Este modo é essencial para o funcionamento do MQTTConsumer, pois ele precisa se conectar a um roteador WiFi para acessar a rede e se comunicar com o broker MQTT, garantindo que o dispositivo possa realizar suas funções de consumo de dados em um ambiente de rede típico.
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config)); // Configura os parâmetros de conexão WiFi para a interface Station, usando a estrutura de configuração preenchida com o SSID e a senha. Esta configuração é crucial para garantir que o dispositivo possa se autenticar e se conectar à rede WiFi correta, permitindo que o MQTTConsumer estabeleça uma conexão de rede bem-sucedida e possa se comunicar com o broker MQTT para realizar suas funções de consumo de dados.
    ESP_ERROR_CHECK(esp_wifi_start()); // Inicia o driver WiFi, permitindo que o dispositivo comece a procurar e se conectar à rede WiFi configurada. Esta função é essencial para ativar o subsistema WiFi e permitir que o MQTTConsumer estabeleça uma conexão de rede, garantindo que o dispositivo possa acessar a rede e se comunicar com o broker MQTT para realizar suas funções de consumo de dados.

    ESP_LOGI(TAG, "WiFi iniciado — à espera de ligação...");
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY); // Aguarda até que o bit de conexão WiFi seja definido, garantindo que o MQTTConsumer só continue sua execução após estabelecer uma conexão de rede bem-sucedida. Esta sincronização é crucial para evitar tentativas de comunicação com o broker MQTT antes que a conectividade de rede esteja disponível, garantindo uma operação mais robusta e evitando erros relacionados à falta de conectividade.
    ESP_LOGI(TAG, "WiFi ligado");
}

// mqttInit 
void MQTTConsumer::mqttInit()
{
    esp_mqtt_client_config_t mqtt_cfg = {}; // Estrutura de configuração para o cliente MQTT, que inclui os parâmetros necessários para se conectar a um broker MQTT específico. Esta estrutura é preenchida com o URI do broker fornecido ao criar o MQTTConsumer, permitindo que o dispositivo saiba para onde enviar as mensagens MQTT, garantindo que o MQTTConsumer possa estabelecer uma conexão com o broker e realizar suas funções de consumo de dados.
    mqtt_cfg.broker.address.uri = broker_uri; // Configura o URI do broker MQTT na estrutura de configuração, permitindo que o cliente MQTT saiba para onde se conectar. O URI inclui o protocolo (mqtt://), o endereço IP ou hostname do broker e a porta, garantindo que o MQTTConsumer possa estabelecer uma conexão com o broker MQTT correto e realizar suas funções de consumo de dados.

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg); // Inicializa o cliente MQTT com a configuração especificada, preparando o sistema para se conectar ao broker MQTT. Esta função é essencial para configurar o cliente MQTT e garantir que ele esteja pronto para estabelecer uma conexão com o broker, permitindo que o MQTTConsumer realize suas funções de consumo de dados e publique mensagens no tópico configurado.
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqttEventHandler, this)); // Registra um handler para eventos relacionados ao MQTT, permitindo que o MQTTConsumer responda a mudanças no estado da conexão MQTT, como conexões, desconexões, publicações e erros. Este handler é essencial para implementar uma lógica de reconexão automática e para monitorar o estado da conexão MQTT, garantindo que o MQTTConsumer possa operar de forma robusta mesmo em condições de rede instáveis e possa reagir adequadamente a eventos relacionados ao MQTT.
    ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt_client));

    ESP_LOGI(TAG, "MQTT cliente iniciado → %s", broker_uri);
}

//  wifiEventHandler 
void MQTTConsumer::wifiEventHandler(void *arg, esp_event_base_t event_base,
                                     int32_t event_id, void *event_data)
{
    MQTTConsumer *self = static_cast<MQTTConsumer*>(arg);

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) { // Quando o WiFi é inicializado, tenta se conectar à rede configurada.
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) { // Se o WiFi for desconectado, sinaliza a desconexão e tenta reconectar automaticamente.
        ESP_LOGW(TAG, "WiFi desligado — a reconectar...");
        esp_wifi_connect();

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) { // Quando o dispositivo obtém um endereço IP, sinaliza a conexão bem-sucedida e define o bit de conexão no grupo de eventos.
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "IP obtido: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(self->wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

//  mqttEventHandler 
void MQTTConsumer::mqttEventHandler(void *arg, esp_event_base_t event_base,
                                     int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED: // Quando o cliente MQTT se conecta ao broker, sinaliza a conexão bem-sucedida.
            ESP_LOGI(TAG, "MQTT ligado ao broker");
            break;
        case MQTT_EVENT_DISCONNECTED: // Quando o cliente MQTT é desconectado do broker, sinaliza a desconexão e tenta reconectar automaticamente.
            ESP_LOGW(TAG, "MQTT desligado");
            break;
        case MQTT_EVENT_PUBLISHED: // Quando uma mensagem é publicada com sucesso, sinaliza a publicação bem-sucedida.
            ESP_LOGI(TAG, "MQTT publicado — msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_ERROR: // Quando ocorre um erro relacionado ao MQTT, sinaliza o erro para depuração e monitoramento.
            ESP_LOGE(TAG, "MQTT erro");
            break;
        default:
            break;
    }
}