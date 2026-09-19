#ifndef DHCP_CORE_RESTSENDERLIMITS_H
#define DHCP_CORE_RESTSENDERLIMITS_H

/**
 * @brief Limits shared by the four modules that post records to an external
 *        REST service.
 *
 * Those modules — DHCP events (`DhcpRestLogger`), the external DNS cache
 * (`DnsCache`, two sender workers), DNS queries (`DnsLogger`) and served time
 * requests (`TimeLogger`) — are five copies of one design, and the numbers that
 * decide how they behave were copies too: the same 6000 ms drain deadline, the
 * same 500 ms queue wait, the same 10 ms poll while the stop marker is handed
 * over and the same 1024-byte HTTP client buffers sat in five files under five
 * names. One value in one place is what this header is for: changing how long a
 * sender waits for its queue to empty becomes one edit instead of five.
 *
 * Only the limits that really are the same live here. Task stacks, queue depths
 * and priorities differ per module on purpose (they depend on what the record
 * holds and on the TLS buffers the sender needs) and stay in their own classes,
 * next to the code that uses them.
 *
 * Nothing here is ESP-IDF specific, so the header is safe to include from a host
 * test as well.
 */
namespace dhcp {
namespace core {

/// How long a sender waits for its own queue to empty while it is being stopped.
constexpr int kDrainDeadlineMs = 6000;

/// How long a sender task waits for the next record before it looks at its stop
/// flag again (the same wait is also used while the stop marker is sent).
constexpr int kQueueWaitMs = 500;

/// Poll interval while the stop marker travels to the sender task.
constexpr int kStopMarkerPollMs = 10;

/// Timeout of one POST to the external service.
constexpr int kSendTimeoutMs = 5000;

/// Receive and transmit buffers of the HTTP client used for that POST.
constexpr int kHttpClientBufferBytes = 1024;

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_RESTSENDERLIMITS_H
