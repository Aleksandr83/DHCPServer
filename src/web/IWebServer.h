#ifndef DHCP_WEB_IWEBSERVER_H
#define DHCP_WEB_IWEBSERVER_H

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
};

} // namespace web
} // namespace dhcp

#endif // DHCP_WEB_IWEBSERVER_H
