#pragma once
#include "Consumer.h"
#include "topic.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

class LCDConsumer : public Consumer {
private:
    // Handle do display
    esp_lcd_panel_handle_t panel = nullptr;

    // Labels LVGL
    lv_obj_t *label_speed    = nullptr;
    lv_obj_t *label_motv_val = nullptr;
    lv_obj_t *label_moti_val = nullptr;
    lv_obj_t *label_mott_val = nullptr;
    lv_obj_t *label_steer_val = nullptr;

    // Últimos valores recebidos — guardados para atualizar o LCD
    float last_voltage = 0.0f;
    float last_current = 0.0f;
    float last_temp    = 0.0f;
    float last_speed   = 0.0f;
    float last_steering = 0.0f;

    // Métodos privados de inicialização
    esp_lcd_panel_handle_t displayInit(); // inicializa o display e retorna o handle
    void lvglInit(esp_lcd_panel_handle_t panel); // inicializa o LVGL e cria os objetos gráficos
    void dashboardCreate(); // cria os elementos gráficos do dashboard (labels, etc.)

    // Callbacks estáticos — necessários porque LVGL e esp_timer
    // não aceitam métodos de instância como callbacks
    static void lvglFlushCb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map); // callback para flush do LVGL. Recebe a área a atualizar e o buffer de pixels, e chama esp_lcd_panel_draw_bitmap() para desenhar no display.
    static void lvglTickCb(void *arg); // callback para o tick do LVGL. Chama lv_tick_inc() a cada x ms para manter o tempo do LVGL atualizado. 

public:
    // Construtor, setup, run e consume
    LCDConsumer() = default;

    void setup()                         override;
    void run()                           override;
    void consume(const topic_t1& topic)   override;
};