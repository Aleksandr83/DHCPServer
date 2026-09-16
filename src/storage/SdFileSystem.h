#ifndef DHCP_STORAGE_SDFILESYSTEM_H
#define DHCP_STORAGE_SDFILESYSTEM_H

#include <string>

#include "IFileSystem.h"

#if CONFIG_IDF_TARGET_ESP32P4
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl.h"
#endif

namespace dhcp {
namespace storage {

/**
 * @brief External microSD volume (`/sdcard`) over the SDMMC peripheral.
 *
 * Implemented for the ESP32-P4 (Waveshare ESP32-P4-ETH: 4-bit SDIO slot) and
 * a no-op stub on any other target — the classic ESP32 build has no card slot,
 * so @ref mount reports "not supported" instead of failing obscurely.
 *
 * Wiring comes from Kconfig (`FILES_SD_PIN_*`), defaulting to the pins of the
 * official Waveshare example: **CLK=43, CMD=44, D0=39, D1=40, D2=41, D3=42**.
 *
 * Mount policy:
 *  - 4-bit first (when `CONFIG_FILES_SD_BUS_WIDTH_4`), then a **1-bit retry**
 *    (a marginal card/wiring often still works 1-bit),
 *  - every attempt gets a fresh `sdmmc_host_t` from `SDMMC_HOST_DEFAULT()` and
 *    the IDF VFS layer releases the host itself when the attempt fails, so the
 *    next one starts from a clean state. The host `flags` are **never**
 *    rewritten: ESP-IDF v6 stores `SDMMC_HOST_FLAG_DEINIT_ARG` there together
 *    with the one-argument `deinit_p`, and clearing it makes the cleanup path
 *    call that function without its slot argument (boot loop). The bus width
 *    is selected through `sdmmc_slot_config_t::width`, not through flags.
 *  - **no formatting**: an unformatted/foreign card simply does not mount, and
 *    the user can format it explicitly through the API,
 *  - optional on-chip LDO power control (`CONFIG_FILES_SD_LDO_CHAN`) for boards
 *    that power the slot from the P4's SD LDO instead of the 3.3 V rail.
 *
 * There is no card-detect GPIO on this board, so "not inserted" and "unusable
 * card" are indistinguishable — @ref FileManager therefore retries the mount
 * periodically (throttled) instead of relying on an interrupt.
 */
class SdFileSystem : public IFileSystem {
public:
    SdFileSystem();
    ~SdFileSystem() override;

    // IFileSystem
    const std::string& id() const override { return id_; }
    const std::string& mountPoint() const override { return mountPoint_; }
    bool mount() override;
    void unmount() override;
    bool isMounted() const override { return mounted_; }
    bool isPresent() const override { return present_; }
    const std::string& lastError() const override { return error_; }
    void verify() override;
    VolumeInfo info() override;
    bool format() override;

private:
    /**
     * @brief One mount attempt with the given bus width.
     * @return true when the card mounted (and the VFS is registered).
     */
    bool tryMount(int busWidth);

    /**
     * @brief Ask the mounted card for its status word (CMD13, `sdmmc_get_status`).
     *
     * Reads nothing from the medium — unlike `f_getfree()`, which FatFs may use
     * to free a broken cluster chain — so the check cannot damage the card.
     * Safe to call while file operations run: the SDMMC host serialises bus
     * transactions on the controller mutex.
     *
     * @return true when the card answered.
     */
    bool probeCard();

    /**
     * @brief Power the slot through the board's switch (see
     * `CONFIG_FILES_SD_PWR_GPIO`).
     *
     * On the Waveshare ESP32-P4-ETH the P-channel MOSFET Q1 (SI2301CDS) sits
     * between the 3.3 V rail and the card supply, its gate driven by GPIO45
     * through a 10 kOhm pull-down — so the slot is powered by default and the
     * firmware only has to keep the pin LOW (the Kconfig default). The driver
     * still flips to the opposite level for the *next* attempt when nothing
     * answers, because a wrong polarity means an unpowered card and that looks
     * exactly like a dead one.
     *
     * @param inverted Use the opposite of the configured polarity.
     */
    void applySlotPower(bool inverted);

    std::string id_;
    std::string mountPoint_;
    std::string error_;
#if CONFIG_IDF_TARGET_ESP32P4
    /** @brief Error of the last single attempt (see @ref mount). */
    std::string attemptError_;
    /** @brief Name of the driver error of the last failed @ref probeCard. */
    std::string probeError_;
    /** @brief Set once the slot configuration has been logged. */
    bool configLogged_ = false;
    /** @brief Level currently driven on the power switch (-1 = none yet). */
    int pwrLevel_ = -1;
    /** @brief Try the opposite switch polarity on the next attempt. */
    bool pwrInverted_ = false;
    sdmmc_card_t* card_ = nullptr;
    sd_pwr_ctrl_handle_t pwrCtrl_ = nullptr;
#endif
    bool mounted_ = false;
    bool present_ = false;
};

} // namespace storage
} // namespace dhcp

#endif // DHCP_STORAGE_SDFILESYSTEM_H
