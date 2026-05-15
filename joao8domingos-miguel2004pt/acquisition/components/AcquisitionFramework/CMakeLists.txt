file(GLOB_RECURSE sources "src/*.cpp" "src/*.c")

idf_component_register(
    SRCS ${sources}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES "esp_adc" "freertos"
    REQUIRES mqtt esp_lcd  
    REQUIRES mqtt esp_lcd esp_wifi esp_adc esp_timer
    REQUIRES mqtt esp_lcd esp_wifi esp_adc nvs_flash driver
)