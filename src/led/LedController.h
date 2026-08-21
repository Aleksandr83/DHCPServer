#ifndef DHCP_LED_LEDCONTROLLER_H
#define DHCP_LED_LEDCONTROLLER_H

#include "ILedController.h"
#include <cstdint>

namespace dhcp {
namespace led {

/**
 * @brief GPIO LED controller implementation.
 *
 * Controls an LED connected to a GPIO pin (default GPIO 26 / D26).
 * Active high by default.
 */
class LedController : public ILedController {
public:
    /**
     * @param gpioPin   GPIO pin number (default 26).
     * @param activeHigh true = GPIO high turns LED on (default).
     */
    explicit LedController(uint8_t gpioPin = 26, bool activeHigh = true);
    ~LedController() override;

    void turnOn() override;
    void turnOff() override;
    bool isOn() const override;
    void toggle() override;

private:
    uint8_t gpioPin_;
    bool activeHigh_;
    bool on_ = false;
};

} // namespace led
} // namespace dhcp

#endif // DHCP_LED_LEDCONTROLLER_H
