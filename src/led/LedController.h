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
 * Active high by default. Passing -1 disables the LED (no-op) for
 * boards without a controllable user LED (e.g. ESP32-P4-ETH).
 */
class LedController : public ILedController {
public:
    /** @brief The GPIO the status LED of this board sits on (rule 39). */
    static constexpr int kDefaultLedGpio = 26;

    /**
     * @param gpioPin   GPIO pin number (default kDefaultLedGpio, or -1 to
     *                  disable the LED entirely).
     * @param activeHigh true = GPIO high turns LED on (default).
     */
    explicit LedController(int gpioPin = kDefaultLedGpio, bool activeHigh = true);
    ~LedController() override;

    void turnOn() override;
    void turnOff() override;
    bool isOn() const override;
    void toggle() override;

private:
    int gpioPin_;
    bool activeHigh_;
    bool on_ = false;
};

} // namespace led
} // namespace dhcp

#endif // DHCP_LED_LEDCONTROLLER_H
