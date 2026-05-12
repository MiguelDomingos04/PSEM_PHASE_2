#pragma once
#include "Producer.h"
#include "esp_adc/adc_continuous.h"
#include "freertos/task.h"

class VoltageProducer : public Producer {
private:
    bool  useMockData;
    float current_value;

    adc_continuous_handle_t adc_handle  = nullptr;
    TaskHandle_t            task_handle = nullptr;

    // 
    static bool IRAM_ATTR onConvDone(adc_continuous_handle_t handle, const adc_continuous_evt_data_t *edata, void *user_data);

    void  setupADC();
    void  readADC();   // envia um topic_t por amostra

    float randomWalk(float current, float maxDelta, float minVal, float maxVal);
    float randomFloatInRange(float minVal, float maxVal);

public:
    explicit VoltageProducer(bool useMockData = true);

    void setup() override;
    void run()   override;
};