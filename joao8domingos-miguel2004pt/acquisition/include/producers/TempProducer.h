#pragma once
#include "Producer.h"
#include "esp_adc/adc_continuous.h"
#include "freertos/task.h"

class TempProducer : public Producer {
private:
    bool useMockData;
    float current_value;

    adc_continuous_handle_t adc_handle  = nullptr;
    TaskHandle_t            task_handle = nullptr;

    // ISR callback para quando um frame de ADC estiver prontas — notifica a task para ler o buffer
    static bool IRAM_ATTR onConvDone(adc_continuous_handle_t handle,
                                     const adc_continuous_evt_data_t *edata,
                                     void *user_data);

    void  setupADC();
    void  readADC();   
    float steinhartHart(float raw); // Converte o valor bruto do ADC para temperatura usando a fórmula de Steinhart-Hart, que é uma aproximação comum para termistores NTC. A fórmula é: T = 1 / (A + B*ln(R) + C*(ln(R))^3) - 273.15, onde R é a resistência do termistor calculada a partir do valor bruto do ADC e os coeficientes A, B e C são específicos do termistor usado. Esta função é usada quando useMockData é false, para obter uma leitura de temperatura realista a partir do sensor.

    float randomWalk(float current, float maxDelta, float minVal, float maxVal);
    float randomFloatInRange(float minVal, float maxVal);

public:
    explicit TempProducer(bool useMockData = true);

    void setup() override;
    void run()   override;
};