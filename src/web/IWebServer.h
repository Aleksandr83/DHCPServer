#ifndef DHCP_WEB_IWEBSERVER_H
#define DHCP_WEB_IWEBSERVER_H

#include <string>

namespace dhcp {
namespace web {

/**
 * @brief Abstract web server interface.
 */
class IWebServer {
public:
    virtual ~IWebServer() = default;

    /**
     * @brief Start the HTTP server.
     * @return true on success.
     */
    virtual bool start() = 0;

    /**
     * @brief Stop the HTTP server.
     */
    virtual void stop() = 0;

    /**
     * @brief Check if server is running.
     */
    virtual bool isRunning() const = 0;

    /**
     * @brief Can the HTTPS server serve with the certificate that is stored?
     *
     * The answer is what the settings API needs before it accepts a request to
     * turn HTTPS on: a server without a usable certificate does not start, and
     * saying yes to a switch that cannot work is how an interface starts lying.
     *
     * @param[out] status Machine-readable reason when the answer is false — one
     *                    of the names of `security::certStatusName`; empty when
     *                    the answer is true (or when HTTPS is not supported at
     *                    all, which the same call reports as false).
     * @return true when the stored pair can serve HTTPS right now.
     */
    virtual bool httpsAvailable(std::string* status = nullptr) const = 0;

    /** @brief Is the HTTPS server serving right now? */
    virtual bool httpsEnabled() const = 0;

    /**
     * @brief Turn the HTTPS server on or off on the running device.
     *
     * @param[in]  enabled The new state; the setting is written to NVS as well,
     *                     so it survives a reboot.
     * @param[out] detail  Why it was refused ("" on success).
     * @return true when the server is in the requested state afterwards.
     */
    virtual bool setHttpsEnabled(bool enabled, std::string* detail = nullptr) = 0;
};

} // namespace web
} // namespace dhcp

#endif // DHCP_WEB_IWEBSERVER_H
