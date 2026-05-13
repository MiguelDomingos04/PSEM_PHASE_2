#pragma once
#include "Producer.h"
#include "driver/mcpwm_cap.h"
#include "freertos/queue.h"

class SpeedProducer : public Producer {
private:
    bool useMockData;
    int  current_rpm;

    // Hardware MCPWM
    mcpwm_cap_timer_handle_t  cap_timer   = nullptr;
    mcpwm_cap_channel_handle_t cap_chan   = nullptr;
    QueueHandle_t              meas_queue = nullptr;

    // Estrutura de medição — igual à Phase 1
    struct CaptureMeas {
        uint32_t period_ticks;
        uint32_t high_ticks;
    };

    // ISR callback — static porque ISRs não aceitam métodos de instância
    static bool IRAM_ATTR capCallback(mcpwm_cap_channel_handle_t cap_chan, const mcpwm_capture_event_data_t *edata, void *user_data);

    void  setupMCPWM();
    float readSpeed();

    int   randomWalkInt(int current, int maxDelta, int minVal, int maxVal);
    float rpmToKmh(int rpm);

public:
    explicit SpeedProducer(bool useMockData = true);

    void setup() override;
    void run()   override;
};