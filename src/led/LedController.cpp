#include "LedController.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char* TAG = "LedController";

namespace dhcp {
namespace led {

LedController::LedController(int gpioPin, bool activeHigh)
    : gpioPin_(gpioPin)
    , activeHigh_(activeHigh)
{
    if (gpioPin_ < 0) {
        ESP_LOGI(TAG, "LED disabled (no user LED on this board)");
        return;
    }
    // Configure GPIO
    gpio_reset_pin(static_cast<gpio_num_t>(gpioPin_));
    gpio_set_direction(static_cast<gpio_num_t>(gpioPin_), GPIO_MODE_OUTPUT);
    turnOff();
    ESP_LOGI(TAG, "LED initialized on GPIO%d (active %s)",
             gpioPin_, activeHigh_ ? "high" : "low");
}

LedController::~LedController()
{
    turnOff();
}

void LedController::turnOn()
{
    on_ = true;
    if (gpioPin_ < 0) return;
    gpio_set_level(static_cast<gpio_num_t>(gpioPin_), activeHigh_ ? 1 : 0);
}

void LedController::turnOff()
{
    on_ = false;
    if (gpioPin_ < 0) return;
    gpio_set_level(static_cast<gpio_num_t>(gpioPin_), activeHigh_ ? 0 : 1);
}

bool LedController::isOn() const
{
    return on_;
}

void LedController::toggle()
{
    if (on_) {
        turnOff();
    } else {
        turnOn();
    }
}

} // namespace led
} // namespace dhcp
