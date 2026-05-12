#pragma once
#include "Producer.h"
#include "driver/spi_master.h"

class SteeringAngleProducer : public Producer {
private:
    bool useMockData;
    float current_value;

    // Hardware SPI
    spi_device_handle_t spi_handle = nullptr;

    void     setupSPI();
    float    readAngle();
    bool     amt22bCheck(uint16_t resp);

    float randomWalk(float current, float maxDelta, float minVal, float maxVal);
    float randomFloatInRange(float minVal, float maxVal);

public:
    explicit SteeringAngleProducer(bool useMockData = true);

    void setup() override;
    void run()   override;
};