/*
 * ENC28J60 driver logging settings (components/enc28j60).
 *
 * Controls verbose per-packet / periodic diagnostics that the ENC28J60 driver
 * prints to the terminal:
 *   - "RX: got pkt len=..."          (received frame)
 *   - "RX frame: dst=... src=... type=0x..."  (frame header dump)
 *   - "  ARP: op=... tgt=..."        (ARP payload dump)
 *   - "TX: len=... dst=... type=0x..."        (transmitted frame)
 *   - "DIAG: EIE=... EIR=... ..."    (periodic register diagnostics, ~1s/10s)
 *
 * These logs are compiled in ONLY when ENC28J60_PKT_LOG_ENABLED is defined
 * (the driver code uses #ifdef/#endif). Disabled by default so the terminal
 * stays quiet and the driver has less overhead.
 *
 * To ENABLE packet logging, either:
 *   1. Uncomment the #define below, or
 *   2. Add `-DENC28J60_PKT_LOG_ENABLED` to board_build.build_flags in
 *      platformio.ini (per environment).
 */
/* #define ENC28J60_PKT_LOG_ENABLED */

#ifndef ENC28J60_LOGGING_H
#define ENC28J60_LOGGING_H

#endif /* ENC28J60_LOGGING_H */
