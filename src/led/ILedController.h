#ifndef DHCP_LED_ILEDCONTROLLER_H
#define DHCP_LED_ILEDCONTROLLER_H

namespace dhcp {
namespace led {

/**
 * @brief Abstract LED controller interface.
 */
class ILedController {
public:
    virtual ~ILedController() = default;

    /**
     * @brief Turn the LED on.
     */
    virtual void turnOn() = 0;

    /**
     * @brief Turn the LED off.
     */
    virtual void turnOff() = 0;

    /**
     * @brief Check if LED is currently on.
     */
    virtual bool isOn() const = 0;

    /**
     * @brief Toggle LED state.
     */
    virtual void toggle() = 0;
};

} // namespace led
} // namespace dhcp

#endif // DHCP_LED_ILEDCONTROLLER_H
