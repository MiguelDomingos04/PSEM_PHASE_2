#include "PerformanceMetricsProducer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <cstdint>

#define TAG "PerformanceMetrics"

PerformanceMetricsProducer::PerformanceMetricsProducer(uint32_t sample_ms): Producer(PRODUCER_ID_CPU_USAGE), sample_ms(sample_ms)
{}

void PerformanceMetricsProducer::setup()
{
    ESP_LOGI(TAG, "PerformanceMetricsProducer inicializado — sample=%lums", 
             (unsigned long)sample_ms);
}

// run 
// Corre a uma frequência muito mais baixa que os sensores primários
// Envia um topic_t por cada métrica
void PerformanceMetricsProducer::run()
{
    // Guardar o tick antes do delay — para medir tick health
    //xTaskGetTickCount() devolve o numero de ticks que passaram desde que a task FreeRTOS foi inicializada. Multiplicando pelo valor de portTICK_PERIOD_MS, obtemos o tempo em milissegundos desde o início da task. Armazenamos este valor em last_wake_tick para comparar com o próximo tick e calcular a saúde dos ticks.
    if (first_run) {
        last_wake_tick = xTaskGetTickCount(); // inicializa o last_wake_tick na primeira execução para evitar um delta grande na primeira medição
        first_run      = false;
    }

    vTaskDelay(pdMS_TO_TICKS(sample_ms)); // espera pelo próximo ciclo de amostragem

    // 1. CPU Usage
    sendMetric(PRODUCER_ID_CPU_USAGE, estimateCpuUsage());

    // 2. Queue Size
    sendMetric(PRODUCER_ID_QUEUE_SIZE, getQueueSize());

    // 3. Tick Health
    sendMetric(PRODUCER_ID_TICK_HEALTH, getTickHealth());
}

// sendMetric 
// Cria um topic_t com a métrica e coloca na queue central
void PerformanceMetricsProducer::sendMetric(uint8_t metric_id, float value)
{
    topic_t topic = {
        .producer_id  = metric_id,
        .timestamp_us = static_cast<uint64_t>(esp_timer_get_time()),
        .value        = value,
    };

    // Envia o topic_t para a queue central do TelemetryManager. Se a queue estiver cheia, descarta a métrica e loga um aviso.
    if (xQueueSend(destinationQueue, &topic, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Queue cheia — métrica 0x%02X descartada", metric_id);
    }

    ESP_LOGI(TAG, "Métrica 0x%02X = %.2f", metric_id, value);
}

// estimateCpuUsage 
// Estima a carga do CPU usando o idle tick counter do FreeRTOS.
// O idle hook incrementa um contador sempre que o CPU está sem trabalho.
// CPU Usage (%) = 100 - (idle_ticks / total_ticks × 100)
float PerformanceMetricsProducer::estimateCpuUsage()
{
    static uint32_t last_idle_ticks  = 0; // Guarda o valor do contador de idle ticks na última medição para calcular o delta
    static uint32_t last_total_ticks = 0; // Guarda o valor do total de ticks na última medição para calcular o delta

    uint32_t idle_ticks  = ulTaskGetIdleRunTimeCounter(); // Obtém o número total de ticks que o CPU passou no estado idle desde o início do sistema. Este valor é incrementado pelo idle hook do FreeRTOS, que é chamado sempre que o CPU não tem tarefas para executar. O contador de idle ticks é uma métrica útil para estimar a carga do CPU, pois quanto mais tempo o CPU passar em idle, menor será a utilização do CPU.
    uint32_t total_ticks = (uint32_t)(esp_timer_get_time() / 1000); // Converte o tempo total desde o início do sistema de microssegundos para milissegundos, e depois para ticks. Isto dá-nos o número total de ticks que passaram desde o início do sistema, o que é necessário para calcular a porcentagem de tempo que o CPU passou em idle.

    uint32_t delta_idle  = idle_ticks  - last_idle_ticks; // Calcula o número de ticks que o CPU passou em idle desde a última medição, subtraindo o valor anterior do contador de idle ticks do valor atual. Este delta representa o tempo que o CPU passou em idle durante o intervalo entre as duas medições.
    uint32_t delta_total = total_ticks - last_total_ticks; // Calcula o número total de ticks que passaram desde a última medição, subtraindo o valor anterior do total de ticks do valor atual. Este delta representa o tempo total que passou durante o intervalo entre as duas medições.

    last_idle_ticks  = idle_ticks; // Atualiza o last_idle_ticks para a próxima medição, armazenando o valor atual do contador de idle ticks.
    last_total_ticks = total_ticks; // Atualiza o last_total_ticks para a próxima medição, armazenando o valor atual do total de ticks.

    if (delta_total == 0) return 0.0f; // Evita divisão por zero, embora istoseja quase impossivel visto que o numero de ticks deve aumentar semrpre entre divisões

    float idle_pct = ((float)delta_idle / (float)delta_total) * 100.0f; // Calcula a porcentagem de tempo que o CPU passou em idle durante o intervalo entre as duas medições, dividindo o delta de idle ticks pelo delta total de ticks e multiplicando por 100 para obter uma porcentagem. 
    float cpu_pct  = 100.0f - idle_pct;

    if (cpu_pct < 0.0f)   cpu_pct = 0.0f; // Em casos raros, devido a timing ou inconsistências no contador de idle ticks, o cálculo pode resultar em um valor negativo para cpu_pct. Isto pode acontecer se, por exemplo, o contador de idle ticks for resetado ou se houver um atraso significativo entre as medições. Para garantir que a métrica de CPU Usage seja sempre representada como um valor válido entre 0% e 100%, limitamos cpu_pct a um mínimo de 0.0f.
    if (cpu_pct > 100.0f) cpu_pct = 100.0f; // De maneira similar, embora seja improvável, o cálculo pode resultar num valor maior que 100% devido a inconsistências ou erros no contador de idle ticks. Para garantir que a métrica do CPU Usage seja sempre representada como um valor válido entre 0% e 100%, limitamos cpu_pct a um máximo de 100.0f.

    return cpu_pct;
}

// getQueueSize 
// Monitoriza o tamanho da queue central do TelemetryManager.
// Se crescer consistentemente, o sistema está a produzir mais do que consome.
float PerformanceMetricsProducer::getQueueSize()
{
    if (destinationQueue == nullptr) return -1.0f; // Se a queue não foi configurada, retorna -1 para indicar que a métrica não está disponível. Isto ajuda a diferenciar entre uma queue vazia (0) e uma queue não configurada (-1).
    return (float)uxQueueMessagesWaiting(destinationQueue); // Retorna o número de mensagens atualmente na queue central do TelemetryManager.
}

// getTickHealth 
// Mede o desvio entre o tempo esperado e o tempo real entre execuções.
// 0.0 = sem atraso, valores positivos = deadline missed em ms
float PerformanceMetricsProducer::getTickHealth()
{
    TickType_t now          = xTaskGetTickCount(); // lÊ o numero de ticks total desde o boot
    TickType_t elapsed_ms   = (now - last_wake_tick) * portTICK_PERIOD_MS; //Calcula o tempo que decorreu desde a ultima vez que esta função foi executada
    float      deviation_ms = (float)elapsed_ms - (float)sample_ms; // Calcula o desvio entre o tempo real decorrido (elapsed_ms) e o tempo esperado (sample_ms). Se o valor for positivo, significa que houve um atraso em relação ao tempo esperado, indicando que a task não conseguiu cumprir o deadline. Se for zero, significa que a task executou exatamente no tempo esperado. Se for negativo, significa que a task executou antes do tempo esperado, o que geralmente não é um problema para a saúde dos ticks.

    last_wake_tick = now; // Atualiza o last_wake_tick para a próxima medição, de modo a armazenar o valor atual do tick count. Isto é importante para garantir que a próxima vez que esta função for chamada, o cálculo do elapsed_ms seja baseado no tempo correto desde a última execução.

    return deviation_ms > 0.0f ? deviation_ms : 0.0f; // Retorna o desvio em milissegundos se for positivo, indicando um atraso, ou 0.0f se não houve atraso. Esta métrica ajuda a monitorar a saúde dos ticks e a identificar se a task está conseguindo cumprir seus deadlines de execução. Se os valores de tick health forem consistentemente altos, pode indicar que o sistema está sobrecarregado e não consegue processar as tarefas em tempo hábil.
}