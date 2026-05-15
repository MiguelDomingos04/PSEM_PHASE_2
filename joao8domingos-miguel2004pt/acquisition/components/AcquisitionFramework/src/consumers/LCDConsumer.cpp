#include "consumers/LCDConsumer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_ili9341.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <cstdio>

#define TAG          "LCDConsumer"

// Pinos
#define PIN_MOSI   11
#define PIN_SCLK   12
#define PIN_CS     10
#define PIN_DC      9
#define PIN_RST    -1
#define PIN_BL      2
#define PIN_MISO   13

// Resolução
#define LCD_H_RES  320
#define LCD_V_RES  240

// LVGL
#define LVGL_TICK_MS  5

// Cores
#define DASH_BLUE  lv_color_make(0x4F, 0xC3, 0xF7)
#define DASH_WHITE lv_color_make(0xFF, 0xFF, 0xFF)
#define DASH_BLACK lv_color_make(0x00, 0x00, 0x00)


// setup 
// Inicializa o display e o LVGL e cria o dashboard
void LCDConsumer::setup()
{
    panel = displayInit();
    lvglInit(panel);
    dashboardCreate();
    ESP_LOGI(TAG, "LCDConsumer iniciado");
}


// run 
// Processa os eventos LVGL pendentes — deve ser chamado regularmente
void LCDConsumer::run()
{
    lv_timer_handler(); // Processa os eventos pendentes do LVGL, como atualizações de ecrã, animações e interações do user. Este método deve ser chamado regularmente no loop principal do consumer para garantir que a interface gráfica permaneça responsiva e atualizada com as últimas informações recebidas dos producers.
    vTaskDelay(pdMS_TO_TICKS(20)); // Aguardar um curto período para evitar consumir 100% da CPU. O LVGL é eficiente, mas chamar lv_timer_handler() com muita frequência pode levar a um uso excessivo da CPU, especialmente se houver muitas atualizações ou animações. Um delay de 20 ms é um bom compromisso para manter a interface responsiva sem sobrecarregar o sistema.
}


// consume 
// Recebe umtopic_t1 da queue e atualiza o label correspondente no dashboard
void LCDConsumer::consume(const topic_t1 &topic)
{
    char buf[32];

    switch (topic.producer_id) {
        case PRODUCER_ID_VOLTAGE:
            last_voltage = topic.value;
            snprintf(buf, sizeof(buf), "%.1fV", last_voltage);
            lv_label_set_text(label_motv_val, buf); // Atualiza o label de tensão no dashboard com o novo valor recebido. A função lv_label_set_text() é usada para definir o texto do label, e snprintf() é utilizado para formatar a string com o valor da tensão, garantindo que seja exibida de forma legível e consistente no display.
            break;

        case PRODUCER_ID_CURRENT:
            last_current = topic.value;
            snprintf(buf, sizeof(buf), "%.1fA", last_current);
            lv_label_set_text(label_moti_val, buf); // Atualiza o label de corrente no dashboard com o novo valor recebido. A função lv_label_set_text() é usada para definir o texto do label, e snprintf() é utilizado para formatar a string com o valor da corrente, garantindo que seja exibida de forma legível e consistente no display.
            break;

        case PRODUCER_ID_TEMP:
            last_temp = topic.value;
            snprintf(buf, sizeof(buf), "%.1fC", last_temp);
            lv_label_set_text(label_mott_val, buf); // Atualiza o label de temperatura no dashboard com o novo valor recebido. A função lv_label_set_text() é usada para definir o texto do label, e snprintf() é utilizado para formatar a string com o valor da temperatura, garantindo que seja exibida de forma legível e consistente no display.
            break;

        case PRODUCER_ID_SPEED:
            last_speed = topic.value;
            snprintf(buf, sizeof(buf), "%.1f Km/h", last_speed);
            lv_label_set_text(label_speed, buf); // Atualiza o label de velocidade no dashboard com o novo valor recebido. A função lv_label_set_text() é usada para definir o texto do label, e snprintf() é utilizado para formatar a string com o valor da velocidade, garantindo que seja exibida de forma legível e consistente no display.
            break;
        case PRODUCER_ID_STEERING:
            last_steering = topic.value;
            snprintf(buf, sizeof(buf), "%.1f°", last_steering);
            lv_label_set_text(label_steer_val, buf); // Atualiza o label de ângulo de direção no dashboard com o novo valor recebido. A função lv_label_set_text() é usada para definir o texto do label, e snprintf() é utilizado para formatar a string com o valor do ângulo de direção, garantindo que seja exibida de forma legível e consistente no display.
            break;
        case PRODUCER_ID_CPU_CORE_0:
        case PRODUCER_ID_CPU_CORE_1:
        case PRODUCER_ID_QUEUE_SIZE:
        case PRODUCER_ID_TICK_HEALTH:
            break;
        default:
            break;
    }
}


// displayInit 
esp_lcd_panel_handle_t LCDConsumer::displayInit()
{
    spi_bus_config_t buscfg = {
        .mosi_io_num     = PIN_MOSI, // GPIO para o sinal de dados MOSI do SPI, usado para enviar os dados de pixel do microcontrolador para o display. Este pino é essencial para a comunicação SPI, permitindo que o microcontrolador controle o conteúdo exibido no LCD.
        .miso_io_num     = PIN_MISO, // GPIO para o sinal de dados MISO do SPI, usado para receber dados do display para o microcontrolador. Embora muitos displays LCD não enviem dados de volta, este pino é configurado para garantir compatibilidade com displays que possam usar comunicação bidirecional ou para futuras expansões do projeto.
        .sclk_io_num     = PIN_SCLK, // GPIO para o sinal de clock SCLK do SPI, usado para sincronizar a comunicação entre o microcontrolador e o display. O clock é fundamental para garantir que os dados sejam transmitidos corretamente, permitindo que o display saiba quando ler os bits de dados enviados pelo microcontrolador.
        .quadwp_io_num   = -1, // GPIO para o sinal de Write Protect (WP) em modo Quad SPI. Este pino não é usado neste projeto, pois o display não requer proteção contra escrita, mas é configurado como -1 para indicar que não está conectado.
        .quadhd_io_num   = -1, // GPIO para o sinal de Hold (HD) em modo Quad SPI. Este pino não é usado neste projeto, pois o display não requer a funcionalidade de hold, mas é configurado como -1 para indicar que não está conectado.
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * 2 + 8, // Tamanho máximo de transferência em bytes para o SPI, calculado com base na resolução do display (320x240) e na profundidade de cor (16 bits por pixel, ou 2 bytes por pixel), mais um pequeno overhead para comandos. Este valor garante que o buffer de transferência seja grande o suficiente para enviar um ecrã inteiro de dados de pixel numa única transação SPI, otimizando a atualização do display e garantindo uma performance suave.
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle;

    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.dc_gpio_num       = PIN_DC;   // GPIO para o sinal de Data/Command (DC) do display, usado para indicar se os dados enviados via SPI são comandos de controle ou dados de pixel. Este pino é crucial para a operação do display, permitindo que o microcontrolador controle o comportamento do LCD e atualize o conteúdo exibido corretamente.
    io_config.cs_gpio_num       = PIN_CS;   // GPIO para o sinal de Chip Select (CS) do SPI, usado para selecionar o display como o dispositivo ativo na linha SPI. Este pino é essencial para garantir que as comunicações SPI sejam direcionadas corretamente ao display, especialmente se houver outros dispositivos SPI conectados ao mesmo barramento.
    io_config.pclk_hz           = 40 * 1000 * 1000; // Frequência do clock SPI, definida para 40 MHz para garantir uma comunicação rápida entre o microcontrolador e o display. Esta configuração é importante para otimizar a taxa de atualização do display, permitindo que os dados de pixel sejam enviados rapidamente e garantindo uma experiência visual fluida.
    io_config.lcd_cmd_bits      = 8; // Número de bits para os comandos enviados ao display, definido como 8 bits, o que é comum para a maioria dos displays LCD. Esta configuração garante que os comandos de controle sejam interpretados corretamente pelo display, permitindo que o microcontrolador configure e controle o comportamento do LCD de forma eficaz.
    io_config.lcd_param_bits    = 8; // Número de bits para os parâmetros enviados ao display, definido como 8 bits, o que é comum para a maioria dos displays LCD. Esta configuração garante que os parâmetros de controle sejam interpretados corretamente pelo display, permitindo que o microcontrolador configure e controle o comportamento do LCD de forma eficaz.
    io_config.spi_mode          = 0; // Modo SPI, definido como 0 (CPOL=0, CPHA=0), que é compatível com a maioria dos displays LCD. Esta configuração garante que a comunicação SPI seja sincronizada corretamente entre o microcontrolador e o display, evitando problemas de timing e garantindo uma transmissão de dados confiável.
    io_config.trans_queue_depth     = 10; // Profundidade da fila de transações SPI, definida como 10 para permitir múltiplas transações pendentes, o que pode melhorar a performance ao enviar grandes quantidades de dados para o display. Esta configuração é importante para garantir que o sistema possa lidar com picos de atualização do display sem perder dados ou causar atrasos perceptíveis na interface gráfica.   

    /*
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num       = PIN_DC,   // GPIO para o sinal de Data/Command (DC) do display, usado para indicar se os dados enviados via SPI são comandos de controle ou dados de pixel. Este pino é crucial para a operação do display, permitindo que o microcontrolador controle o comportamento do LCD e atualize o conteúdo exibido corretamente.
        .cs_gpio_num       = PIN_CS,   // GPIO para o sinal de Chip Select (CS) do SPI, usado para selecionar o display como o dispositivo ativo na linha SPI. Este pino é essencial para garantir que as comunicações SPI sejam direcionadas corretamente ao display, especialmente se houver outros dispositivos SPI conectados ao mesmo barramento.
        .pclk_hz           = 40 * 1000 * 1000, // Frequência do clock SPI, definida para 40 MHz para garantir uma comunicação rápida entre o microcontrolador e o display. Esta configuração é importante para otimizar a taxa de atualização do display, permitindo que os dados de pixel sejam enviados rapidamente e garantindo uma experiência visual fluida.
        .lcd_cmd_bits      = 8, // Número de bits para os comandos enviados ao display, definido como 8 bits, o que é comum para a maioria dos displays LCD. Esta configuração garante que os comandos de controle sejam interpretados corretamente pelo display, permitindo que o microcontrolador configure e controle o comportamento do LCD de forma eficaz.
        .lcd_param_bits    = 8, // Número de bits para os parâmetros enviados ao display, definido como 8 bits, o que é comum para a maioria dos displays LCD. Esta configuração garante que os parâmetros de controle sejam interpretados corretamente pelo display, permitindo que o microcontrolador configure e controle o comportamento do LCD de forma eficaz.
        .spi_mode          = 0, // Modo SPI, definido como 0 (CPOL=0, CPHA=0), que é compatível com a maioria dos displays LCD. Esta configuração garante que a comunicação SPI seja sincronizada corretamente entre o microcontrolador e o display, evitando problemas de timing e garantindo uma transmissão de dados confiável.
        .trans_queue_depth = 10, // Profundidade da fila de transações SPI, definida como 10 para permitir múltiplas transações pendentes, o que pode melhorar a performance ao enviar grandes quantidades de dados para o display. Esta configuração é importante para garantir que o sistema possa lidar com picos de atualização do display sem perder dados ou causar atrasos perceptíveis na interface gráfica.
    };
    */
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle));

    esp_lcd_panel_handle_t panel_handle;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_RST, // GPIO para o sinal de reset do display, usado para reiniciar o display e garantir que ele esteja em um estado conhecido antes de iniciar a operação. Este pino é essencial para a inicialização correta do display.
        .rgb_endian     = LCD_RGB_ENDIAN_BGR, // Define a ordem dos bytes para os dados de pixel RGB, configurada como BGR para compatibilidade com o display ILI9341, que espera os dados de pixel nesta ordem. Esta configuração é crucial para garantir que as cores sejam exibidas corretamente no LCD, evitando inversões de cor ou artefatos visuais.
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(
        io_handle, &panel_config, &panel_handle));

    esp_lcd_panel_reset(panel_handle); // Reseta o display para garantir que ele esteja num estado conhecido antes de iniciar a operação. Este passo é importante para evitar problemas de inicialização e garantir que o display funcione corretamente desde o início.
    esp_lcd_panel_init(panel_handle);  // Inicializa o display, configurando os registros internos e preparando-o para receber dados de pixel. Este passo é essencial para garantir que o display esteja pronto para exibir conteúdo e responda corretamente aos comandos enviados pelo microcontrolador.
    esp_lcd_panel_swap_xy(panel_handle, true); 
    esp_lcd_panel_disp_on_off(panel_handle, true); // Liga o display para que ele comece a exibir o conteúdo enviado. Este passo é crucial para garantir que o display esteja ativo e pronto para mostrar as informações do dashboard assim que os dados de pixel forem enviados.

    gpio_set_direction((gpio_num_t)PIN_BL, GPIO_MODE_OUTPUT); // Configura o pino de controle do backlight como saída. Este pino é usado para controlar a iluminação de fundo do display, permitindo que o microcontrolador ligue ou desligue o backlight conforme necessário para economizar energia ou ajustar a visibilidade do display em diferentes condições de iluminação ambiente.
    gpio_set_level((gpio_num_t)PIN_BL, 1); // Define o nível alto para ligar o backlight do display, garantindo que o conteúdo exibido seja visível. Este passo é importante para garantir que o display esteja iluminado e que as informações do dashboard possam ser vistas claramente pelo user

    ESP_LOGI(TAG, "Display ILI9341 iniciado");
    return panel_handle;
}


// lvglInit 
void LCDConsumer::lvglInit(esp_lcd_panel_handle_t panel)
{
    lv_init(); // Inicializa a biblioteca LVGL, preparando-a para criar e gerir a interface gráfica. Este passo é essencial para configurar os recursos internos do LVGL, como timers, drivers de exibição e gestão de objetos gráficos, garantindo que a biblioteca esteja pronta para ser usada no dashboard do LCD.

    static lv_disp_draw_buf_t draw_buf; // Buffer de desenho para o LVGL, usado para armazenar os dados de pixel que serão enviados para o display. Este buffer é necessário para a operação do LVGL, permitindo que ele prepare os dados de pixel em memória antes de enviá-los para o display, otimizando a performance e garantindo uma atualização suave da interface gráfica.
    static lv_color_t buf[LCD_H_RES * 20]; // Buffer de cor para o LVGL, dimensionado para armazenar os dados de pixel de uma área de 320x20 pixels do display. Este buffer é usado como parte do processo de renderização do LVGL, permitindo que ele prepare os dados de pixel em memória antes de enviá-los para o display, otimizando a performance e garantindo uma atualização suave da interface gráfica.
    lv_disp_draw_buf_init(&draw_buf, buf, nullptr, LCD_H_RES * 20); // Inicializa o buffer de desenho do LVGL, associando o buffer de cor ao driver de exibição. Este passo é crucial para configurar a forma como o LVGL gere os dados de pixel, permitindo que ele use o buffer para preparar as atualizações do display de forma eficiente e garantir uma performance otimizada na renderização da interface gráfica.

    static lv_disp_drv_t disp_drv; // Driver de exibição para o LVGL, usado para configurar a comunicação entre o LVGL e o display. Este driver é essencial para garantir que o LVGL possa enviar os dados de pixel corretamente para o display, permitindo que a interface gráfica seja exibida de forma precisa e responsiva.
    lv_disp_drv_init(&disp_drv);   // Inicializa o driver de exibição do LVGL, preparando-o para ser configurado com os parâmetros específicos do display. Este passo é necessário para garantir que o driver esteja num estado conhecido antes de configurar os detalhes da comunicação com o display, garantindo uma integração suave entre o LVGL e o hardware do LCD.
    disp_drv.hor_res   = LCD_H_RES; // Configura a resolução horizontal do driver de exibição do LVGL, definindo-a como 320 pixels para corresponder à resolução do display. Esta configuração é crucial para garantir que o LVGL saiba as dimensões corretas do display, permitindo que ele gerencie os objetos gráficos e as atualizações de pixel de forma precisa e eficiente.
    disp_drv.ver_res   = LCD_V_RES; // Configura a resolução vertical do driver de exibição do LVGL, definindo-a como 240 pixels para corresponder à resolução do display. Esta configuração é crucial para garantir que o LVGL saiba as dimensões corretas do display, permitindo que ele gerencie os objetos gráficos e as atualizações de pixel de forma precisa e eficiente.
    disp_drv.flush_cb  = lvglFlushCb; // Configura a callback de flush do driver de exibição do LVGL, associando-a ao método estático lvglFlushCb da classe LCDConsumer. Este callback é chamada pelo LVGL sempre que precisa atualizar uma área do display, e é responsável por enviar os dados de pixel para o display usando a API do ESP-IDF, garantindo que as atualizações da interface gráfica sejam refletidas no LCD de forma eficiente e responsiva.
    disp_drv.draw_buf  = &draw_buf; // Associa o buffer de desenho ao driver de exibição do LVGL, permitindo que o LVGL use o buffer para preparar as atualizações do display de forma eficiente. Esta configuração é essencial para garantir que o LVGL possa gerenciar os dados de pixel corretamente e otimizar a performance da renderização da interface gráfica.
    disp_drv.user_data = panel; // Armazena o handle do painel de exibição no campo user_data do driver de exibição do LVGL, permitindo que a callback de flush acesse o handle do painel quando precisar enviar os dados de pixel para o display. Esta configuração é crucial para garantir que a callback de flush possa interagir corretamente com o hardware do display, enviando os dados de pixel para o painel usando a API do ESP-IDF e garantindo uma atualização eficiente da interface gráfica.
    lv_disp_drv_register(&disp_drv); // Registra o driver de exibição do LVGL, tornando-o ativo e permitindo que o LVGL comece a usar o driver para gerir a comunicação com o display. Este passo é essencial para garantir que o LVGL possa enviar os dados de pixel para o display e renderizar a interface gráfica corretamente.

    const esp_timer_create_args_t timer_args = {
        .callback = lvglTickCb, // Callback para o tick do LVGL, associada ao método estático lvglTickCb da classe LCDConsumer. Este callback é chamado periodicamente para manter o tempo do LVGL atualizado, garantindo que as animações e os eventos temporizados do LVGL funcionem corretamente e que a interface gráfica permaneça responsiva.
        .name     = "lvgl_tick", // Nome do timer, usado para identificação e depuração. Este nome é útil para monitorar o timer no sistema e facilitar a identificação de problemas relacionados ao tick do LVGL durante o desenvolvimento e a depuração do projeto.
    };
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, LVGL_TICK_MS * 1000));

    ESP_LOGI(TAG, "LVGL iniciado");
}


// dashboardCreate 
void LCDConsumer::dashboardCreate()
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, DASH_BLACK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    // Velocidade
    label_speed = lv_label_create(scr);
    lv_obj_set_style_text_font(label_speed, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(label_speed, DASH_BLUE, LV_PART_MAIN);
    lv_label_set_text(label_speed, "0.0 Km/h");
    lv_obj_align(label_speed, LV_ALIGN_TOP_MID, 0, 20);

    // MotV
    lv_obj_t *label_motv = lv_label_create(scr);
    lv_obj_set_style_text_font(label_motv, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(label_motv, DASH_WHITE, LV_PART_MAIN);
    lv_label_set_text(label_motv, "MotV");
    lv_obj_align(label_motv, LV_ALIGN_BOTTOM_LEFT, 20, -40);

    label_motv_val = lv_label_create(scr);
    lv_obj_set_style_text_font(label_motv_val, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(label_motv_val, DASH_BLUE, LV_PART_MAIN);
    lv_label_set_text(label_motv_val, "0.0V");
    lv_obj_align(label_motv_val, LV_ALIGN_BOTTOM_LEFT, 20, -20);

    // MotI
    lv_obj_t *label_moti = lv_label_create(scr);
    lv_obj_set_style_text_font(label_moti, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(label_moti, DASH_WHITE, LV_PART_MAIN);
    lv_label_set_text(label_moti, "MotI");
    lv_obj_align(label_moti, LV_ALIGN_BOTTOM_MID, 0, -40);

    label_moti_val = lv_label_create(scr);
    lv_obj_set_style_text_font(label_moti_val, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(label_moti_val, DASH_BLUE, LV_PART_MAIN);
    lv_label_set_text(label_moti_val, "0.0A");
    lv_obj_align(label_moti_val, LV_ALIGN_BOTTOM_MID, 0, -20);

    // MotT
    lv_obj_t *label_mott = lv_label_create(scr);
    lv_obj_set_style_text_font(label_mott, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(label_mott, DASH_WHITE, LV_PART_MAIN);
    lv_label_set_text(label_mott, "MotT");
    lv_obj_align(label_mott, LV_ALIGN_BOTTOM_RIGHT, -20, -40);

    label_mott_val = lv_label_create(scr);
    lv_obj_set_style_text_font(label_mott_val, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(label_mott_val, DASH_BLUE, LV_PART_MAIN);
    lv_label_set_text(label_mott_val, "0.0C");
    lv_obj_align(label_mott_val, LV_ALIGN_BOTTOM_RIGHT, -20, -20);

    // Steering Angle
    lv_obj_t *label_steer = lv_label_create(scr);
    lv_obj_set_style_text_font(label_steer, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(label_steer, DASH_WHITE, LV_PART_MAIN);
    lv_label_set_text(label_steer, "SteerAngle");
    lv_obj_align(label_steer, LV_ALIGN_TOP_LEFT, 20, 20);

    label_steer_val = lv_label_create(scr);
    lv_obj_set_style_text_font(label_steer_val, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(label_steer_val, DASH_BLUE, LV_PART_MAIN);
    lv_label_set_text(label_steer_val, "0.0°");
    lv_obj_align(label_steer_val, LV_ALIGN_TOP_LEFT, 20, 40);
}


// Callbacks estáticos 
// Callback de flush do LVGL, chamado quando o LVGL precisa atualizar uma área do display. Este método é responsável por enviar os dados de pixel para o display usando a API do ESP-IDF, garantindo que as atualizações da interface gráfica sejam refletidas no LCD de forma eficiente e responsiva.
void LCDConsumer::lvglFlushCb(lv_disp_drv_t *drv, const lv_area_t *area,
                               lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
    esp_lcd_panel_draw_bitmap(panel,
                              area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1,
                              color_map);
    lv_disp_flush_ready(drv);
}

// Callback de tick do LVGL, chamado periodicamente para atualizar o timer interno do LVGL. Este método é essencial para garantir que o LVGL possa gerir corretamente os eventos de tempo, como animações e atualizações de ecrã, mantendo a interface gráfica responsiva e fluida.
void LCDConsumer::lvglTickCb(void *arg)
{
    lv_tick_inc(LVGL_TICK_MS);
}