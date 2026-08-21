#include "LedController.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char* TAG = "LedController";

namespace dhcp {
namespace led {

LedController::LedController(uint8_t gpioPin, bool activeHigh)
    : gpioPin_(gpioPin)
    , activeHigh_(activeHigh)
{
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
    gpio_set_level(static_cast<gpio_num_t>(gpioPin_), activeHigh_ ? 1 : 0);
    on_ = true;
}

void LedController::turnOff()
{
    gpio_set_level(static_cast<gpio_num_t>(gpioPin_), activeHigh_ ? 0 : 1);
    on_ = false;
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
