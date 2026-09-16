#include "SdFileSystem.h"

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_IDF_TARGET_ESP32P4
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"   // sdmmc_get_status — the liveness probe (see verify())
#endif

namespace dhcp {
namespace storage {

namespace {
const char* TAG = "SdFileSystem";
const char* kMountPoint = "/sdcard";

#if CONFIG_IDF_TARGET_ESP32P4
/** @brief Max open files on the card (the API opens one at a time). */
constexpr int kMaxFiles = 8;
/** @brief Cluster size for a freshly formatted card (FAT32 friendly). */
constexpr int kAllocUnit = 16 * 1024;

/**
 * @brief Configured polarity of the slot power switch (`CONFIG_FILES_SD_PWR_ACTIVE_HIGH`).
 *
 * A Kconfig `bool` set to `n` is **not defined at all** in `sdkconfig.h`, so the
 * symbol cannot be used as an expression — only `defined()` tells the two states
 * apart. `[[maybe_unused]]` covers the build where the power GPIO is disabled
 * and both uses disappear.
 */
#if defined(CONFIG_FILES_SD_PWR_ACTIVE_HIGH)
[[maybe_unused]] constexpr bool kPwrActiveHigh = true;
#else
[[maybe_unused]] constexpr bool kPwrActiveHigh = false;
#endif
#endif
} // namespace

SdFileSystem::SdFileSystem()
    : id_("sd")
    , mountPoint_(kMountPoint)
{
}

SdFileSystem::~SdFileSystem()
{
    unmount();
#if CONFIG_IDF_TARGET_ESP32P4
    if (pwrCtrl_ != nullptr) {
        sd_pwr_ctrl_del_on_chip_ldo(pwrCtrl_);
        pwrCtrl_ = nullptr;
    }
#endif
}

#if !CONFIG_IDF_TARGET_ESP32P4

// The classic ESP32 board has no card slot: keep the class honest but inert.
bool SdFileSystem::tryMount(int) { return false; }

bool SdFileSystem::mount()
{
    present_ = false;
    error_ = "not supported on this target";
    return false;
}

void SdFileSystem::unmount() { mounted_ = false; }

void SdFileSystem::verify()
{
    // No card slot on this target, so no removable medium either.
}

VolumeInfo SdFileSystem::info()
{
    VolumeInfo out;
    out.id = id_;
    out.mountPoint = mountPoint_;
    out.mounted = false;
    out.present = false;
    out.error = error_;
    return out;
}

bool SdFileSystem::format() { return false; }

bool SdFileSystem::powerCycle(uint32_t)
{
    // No slot, no supply to cut (and nothing to break loose from).
    return false;
}

#else  // CONFIG_IDF_TARGET_ESP32P4

bool SdFileSystem::tryMount(int busWidth, bool formatIfNeeded)
{
    // Fresh host description per attempt.
    //
    // `flags` is deliberately left exactly as SDMMC_HOST_DEFAULT() fills it in.
    // The bus width is **not** selected here (it comes from `slot.width`
    // below), and overwriting the field breaks the IDF cleanup path: the
    // default sets SDMMC_HOST_FLAG_DEINIT_ARG next to `deinit_p =
    // sdmmc_host_deinit_slot` (one argument), so clearing the flag makes
    // call_host_deinit() call that function through the zero-argument `deinit`
    // member of the same union. The slot argument is then garbage,
    // sdmmc_host_deinit_slot() hands it to sd_host_remove_slot() and the chip
    // jumps into unmapped memory — an endless boot loop with
    // `Guru Meditation Error ... Illegal instruction` right after
    // `sdmmc_init_ocr ... returned 0x107` (no card inserted).
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;   // 20 MHz
    host.pwr_ctrl_handle = pwrCtrl_;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = static_cast<gpio_num_t>(CONFIG_FILES_SD_PIN_CLK);
    slot.cmd = static_cast<gpio_num_t>(CONFIG_FILES_SD_PIN_CMD);
    slot.d0  = static_cast<gpio_num_t>(CONFIG_FILES_SD_PIN_D0);
    if (busWidth == 4) {
        slot.d1 = static_cast<gpio_num_t>(CONFIG_FILES_SD_PIN_D1);
        slot.d2 = static_cast<gpio_num_t>(CONFIG_FILES_SD_PIN_D2);
        slot.d3 = static_cast<gpio_num_t>(CONFIG_FILES_SD_PIN_D3);
    }
    slot.width = static_cast<uint8_t>(busWidth);
    slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;  // debug convenience only

    esp_vfs_fat_mount_config_t mcfg = {};
    // Formatting is an explicit action, so a plain mount never creates a
    // filesystem — only `format()` asks for that, to rescue a card whose
    // filesystem is gone.
    mcfg.format_if_mount_failed = formatIfNeeded;
    mcfg.max_files = kMaxFiles;
    mcfg.allocation_unit_size = kAllocUnit;
    mcfg.disk_status_check_enable = false;
    mcfg.use_one_fat = false;

    sdmmc_card_t* card = nullptr;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(mountPoint_.c_str(), &host, &slot,
                                            &mcfg, &card);
    if (err == ESP_OK) {
        // A mount can report success while nothing is mounted: when the VFS path
        // is still registered from an earlier attempt (a failed format leaves it
        // behind), the mount code skips registering again, ends up with a NULL
        // FatFS object and mounts that — which FatFS reads as "unmount". Ask the
        // volume for its size and take such a mount apart again, so the state is
        // honest and the next attempt starts from a clean slate.
        uint64_t total = 0, free = 0;
        if (esp_vfs_fat_info(mountPoint_.c_str(), &total, &free) != ESP_OK) {
            ESP_LOGW(TAG, "%s: mount reported success but the volume is not "
                          "usable — releasing it", id_.c_str());
            esp_vfs_fat_sdcard_unmount(mountPoint_.c_str(), card);
            attemptError_ = std::string("mount left no usable volume (") +
                            (busWidth == 4 ? "4-bit" : "1-bit") + ")";
            return false;
        }
        card_ = card;
        mounted_ = true;
        present_ = true;
        error_.clear();
        // The switch polarity is proven from now on: the slot is powered.
        pwrProven_ = true;
        ESP_LOGI(TAG, "microSD mounted at %s (%d-bit): %s %llu MB",
                 mountPoint_.c_str(), busWidth,
                 card->cid.name,
                 (unsigned long long)((uint64_t)card->csd.capacity *
                                      card->csd.sector_size / (1024 * 1024)));
        return true;
    }

    // Failed: IDF has already released the host on its own.
    // esp_vfs_fat_sdmmc_sdcard_init() runs its own cleanup as soon as the host
    // was initialised (`if (host_inited) call_host_deinit(host_config)`), so the
    // peripheral is free for the next attempt — an extra sdmmc_host_deinit()
    // here would deinitialise it a second time.
    attemptError_ = std::string("mount failed (") + (busWidth == 4 ? "4-bit" : "1-bit") +
                    "): " + esp_err_to_name(err);
    ESP_LOGW(TAG, "%s: %s", id_.c_str(), attemptError_.c_str());
    return false;
}

bool SdFileSystem::mount()
{
    if (mounted_) return true;
    return mountAttempts(false);
}

bool SdFileSystem::mountWithFormat()
{
    if (mounted_) return true;
    return mountAttempts(true);
}

bool SdFileSystem::mountAttempts(bool formatIfNeeded)
{
    // The wiring this attempt will use — logged once, because a wrong value
    // here (menuconfig) looks exactly like broken hardware from the outside.
    if (!configLogged_) {
        configLogged_ = true;
        ESP_LOGI(TAG, "slot pins clk=%d cmd=%d d0=%d d1=%d d2=%d d3=%d%s, ldo=%d, power gpio=%d%s",
                 CONFIG_FILES_SD_PIN_CLK, CONFIG_FILES_SD_PIN_CMD,
                 CONFIG_FILES_SD_PIN_D0, CONFIG_FILES_SD_PIN_D1,
                 CONFIG_FILES_SD_PIN_D2, CONFIG_FILES_SD_PIN_D3,
#if CONFIG_FILES_SD_BUS_WIDTH_4
                 " (4-bit first)",
#else
                 " (1-bit only)",
#endif
                 CONFIG_FILES_SD_LDO_CHAN,
                 CONFIG_FILES_SD_PWR_GPIO,
#if CONFIG_FILES_SD_PWR_GPIO >= 0
                 kPwrActiveHigh ? " (active high)" : " (active low)");
#else
                 "");
#endif
    }

    // Optional on-chip LDO powering the slot (board dependent); created once.
#if CONFIG_FILES_SD_LDO_CHAN >= 0
    if (pwrCtrl_ == nullptr) {
        sd_pwr_ctrl_ldo_config_t ldoCfg = {};
        ldoCfg.ldo_chan_id = CONFIG_FILES_SD_LDO_CHAN;
        if (sd_pwr_ctrl_new_on_chip_ldo(&ldoCfg, &pwrCtrl_) != ESP_OK) {
            ESP_LOGW(TAG, "on-chip LDO %d for the card is unavailable",
                     CONFIG_FILES_SD_LDO_CHAN);
        }
    }
#endif

    // Board switch first: a card without supply stays silent whatever the
    // driver does, so this is not something to skip even on a retry.
    applySlotPower(pwrInverted_);

    // Both attempts are reported, not just the last one: an answer from the card
    // that the driver then rejects (`ESP_ERR_INVALID_RESPONSE`) means something
    // completely different from silence on the CMD line (`ESP_ERR_TIMEOUT`),
    // and with a single line the second attempt used to hide the first.
    std::string notes;
#if CONFIG_FILES_SD_BUS_WIDTH_4
    if (tryMount(4, formatIfNeeded)) return true;
    notes = attemptError_;
#endif
    if (tryMount(1, formatIfNeeded)) return true;
    notes += notes.empty() ? attemptError_ : ("; " + attemptError_);

    error_ = notes;
    present_ = false;

    // Nothing answered with this switch polarity. Flip it for the *next* call
    // (the volume is retried every 5 s) instead of requiring a rebuild: a wrong
    // polarity leaves the card unpowered, and that is indistinguishable from a
    // dead card. Only while the wiring is unproven, though — once a mount has
    // succeeded the polarity is known to be right, and flipping it afterwards
    // would leave the card unpowered on every second attempt (see pwrProven_).
#if CONFIG_FILES_SD_PWR_GPIO >= 0
    if (!pwrProven_) {
        pwrInverted_ = !pwrInverted_;
        ESP_LOGW(TAG, "%s: no answer — switching the slot power to the %s level on the next try",
                 id_.c_str(), pwrInverted_ ? "opposite" : "configured");
    }
#endif
    return false;
}

void SdFileSystem::applySlotPower(bool inverted)
{
#if CONFIG_FILES_SD_PWR_GPIO >= 0
    const bool activeHigh = inverted ? !kPwrActiveHigh : kPwrActiveHigh;
    const int level = activeHigh ? 1 : 0;
    const gpio_num_t pin = static_cast<gpio_num_t>(CONFIG_FILES_SD_PWR_GPIO);

    if (pwrLevel_ == -1) {
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = 1ULL << CONFIG_FILES_SD_PWR_GPIO;
        cfg.mode = GPIO_MODE_OUTPUT;
        cfg.pull_up_en = GPIO_PULLUP_DISABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        cfg.intr_type = GPIO_INTR_DISABLE;
        if (gpio_config(&cfg) != ESP_OK) {
            ESP_LOGW(TAG, "slot power GPIO %d could not be configured",
                     CONFIG_FILES_SD_PWR_GPIO);
            pwrLevel_ = -2;   // stop trying, but do not block the mount
            return;
        }
    }
    if (pwrLevel_ == -2) return;   // configuration failed earlier
    if (pwrLevel_ == level) return;

    // The card must see a stable supply before it answers: the SD specification
    // only asks for ~1 ms after power-up, 20 ms is comfortably inside that and
    // costs nothing once per polarity change.
    gpio_set_level(pin, level);
    pwrLevel_ = level;
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGI(TAG, "slot power GPIO %d driven %s", CONFIG_FILES_SD_PWR_GPIO,
             level ? "high" : "low");
#else
    (void)inverted;
#endif
}

bool SdFileSystem::powerCycle(uint32_t offMs)
{
#if CONFIG_FILES_SD_PWR_GPIO >= 0
    // Make sure the pin is configured before it is used; a card that is not
    // powered at all would make the cut a no-op.
    applySlotPower(pwrInverted_);
    if (pwrLevel_ == -2) {
        error_ = "the card supply switch is not usable";
        return false;
    }

    const bool activeHigh = pwrInverted_ ? !kPwrActiveHigh : kPwrActiveHigh;
    const gpio_num_t pin = static_cast<gpio_num_t>(CONFIG_FILES_SD_PWR_GPIO);
    const int onLevel = activeHigh ? 1 : 0;
    const int offLevel = activeHigh ? 0 : 1;

    ESP_LOGW(TAG, "%s: cutting the card supply for %u ms", id_.c_str(),
             (unsigned)offMs);
    gpio_set_level(pin, offLevel);
    vTaskDelay(pdMS_TO_TICKS(offMs));
    gpio_set_level(pin, onLevel);
    // The SD specification asks for ~1 ms after power-up; the same 20 ms margin
    // the mount path uses.
    vTaskDelay(pdMS_TO_TICKS(20));
    pwrLevel_ = onLevel;

    // The state of this object is deliberately left alone: the call that is stuck
    // in the driver returns with an error in a moment and its own path releases
    // the card, the diskio slot and the SDMMC controller (see format()). Clearing
    // the handle here would take that release away from it — and `error_` is left
    // to that same path, which either reports the failure or clears it.
    ESP_LOGW(TAG, "%s: card supply restored, the card starts over", id_.c_str());
    return true;
#else
    (void)offMs;
    return false;   // this board cannot switch the card's supply
#endif
}

void SdFileSystem::unmount()
{
    if (!mounted_) return;

    esp_err_t err = esp_vfs_fat_sdcard_unmount(mountPoint_.c_str(), card_);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: unmount failed (%s)", id_.c_str(),
                 esp_err_to_name(err));
    }
    card_ = nullptr;
    mounted_ = false;
}

bool SdFileSystem::probeCard()
{
    if (card_ == nullptr) {
        probeError_ = "no card handle";
        return false;
    }

    const esp_err_t err = sdmmc_get_status(card_);
    if (err == ESP_OK) return true;

    probeError_ = esp_err_to_name(err);
    return false;
}

void SdFileSystem::verify()
{
    if (!mounted_) return;

    // A card that was pulled out is only visible on the bus: this board has no
    // card-detect line, and FatFS goes on serving the allocation data it cached
    // at mount time, so without this check the volume would report "mounted"
    // with the capacities from the moment it was mounted — until a reboot.
    if (probeCard()) return;

    // One unanswered request is not proof: this runs from the REST polling path
    // every few seconds, and a single transient bus error must not make the
    // volume vanish from the UI. The repeat is sent immediately, so a card that
    // is really gone is still noticed in the same poll.
    if (probeCard()) return;

    ESP_LOGW(TAG, "%s: card stopped answering (%s) — unmounting the volume",
             id_.c_str(), probeError_.c_str());
    unmount();
    present_ = false;
    error_ = "card removed (" + probeError_ + ")";
}

VolumeInfo SdFileSystem::info()
{
    VolumeInfo out;
    out.id = id_;
    out.mountPoint = mountPoint_;
    out.mounted = mounted_;
    out.present = present_ || mounted_;
    out.error = error_;

    if (mounted_) {
        uint64_t total = 0, free = 0;
        if (esp_vfs_fat_info(mountPoint_.c_str(), &total, &free) == ESP_OK) {
            out.totalBytes = total;
            out.freeBytes = free;
        }
    }
    return out;
}

bool SdFileSystem::format()
{
    // A card whose filesystem is gone cannot be mounted — `format_if_mount_failed`
    // is off for a plain mount on purpose — and refusing to format it would leave
    // the operator with a card nothing can bring back: the format needs a mounted
    // card, the mount needs a filesystem, and only a format could create one.
    // That dead end is easy to reach (an interrupted format is the classic way to
    // lose a filesystem), so the volume is mounted **for formatting** here: the
    // same attempt as a normal mount, except that IDF is allowed to create the
    // filesystem it cannot find.
    if (!mounted_) {
        ESP_LOGW(TAG, "%s: not mounted — mounting it to recreate the filesystem",
                 id_.c_str());
        // A card that still has a usable filesystem mounts first and is then
        // formatted by the normal path below, so the request is never satisfied
        // by a plain mount.
        if (!mount()) {
            ESP_LOGW(TAG, "%s: mounting to format it (the card has no filesystem)",
                     id_.c_str());
            const bool ok = mountWithFormat();
            if (!ok) {
                // error_ carries what the driver said (no card / no answer).
                return false;
            }
            ESP_LOGW(TAG, "%s: filesystem created while mounting", id_.c_str());
            return true;
        }
    }

    ESP_LOGW(TAG, "formatting the microSD card at %s — all data will be lost",
             mountPoint_.c_str());

    // `esp_vfs_fat_sdcard_format()` is a three-step sequence — unmount the FATFS
    // drive, create a new filesystem, mount it back — and it reports only the
    // result of the middle step. When a step fails, the pieces stay where they
    // fell: a mkfs error leaves the volume unmounted with the VFS path still
    // registered, and when the *mount back* fails the helper has already
    // released the card handle, the SDMMC host and the VFS path. The two are
    // indistinguishable from the return value, and getting it wrong means either
    // a card handle that no longer exists (a use-after-free in the liveness
    // probe of verify()) or a mount() that reports success while nothing is
    // mounted (esp_vfs_fat_register answers ESP_ERR_INVALID_STATE for the path
    // that is still registered, which the mount path treats as fine and then
    // mounts a NULL FatFS object — FatFS reads that as "unmount").
    //
    // A card that does not come back is therefore dropped to "not mounted" and
    // the next mount() rebuilds the host, the card and the VFS from scratch.
    esp_err_t err = esp_vfs_fat_sdcard_format(mountPoint_.c_str(), card_);
    if (err != ESP_OK) {
        error_ = std::string("format failed: ") + esp_err_to_name(err);
        ESP_LOGE(TAG, "%s: %s", id_.c_str(), error_.c_str());
        // This branch leaves the card handle, the diskio slot, the VFS path and
        // **the SDMMC controller** exactly as they were (the helper returns
        // without touching them). The controller matters most: the driver claims
        // it during init and `sd_host_claim_controller()` refuses a second
        // claimant, so without releasing it here every later mount() answers
        // "no available sd host controller" and the card is dead until the
        // device is rebooted — which is how a failed format on a marginal card
        // used to end. `esp_vfs_fat_sdcard_unmount` is the full release
        // (FATFS drive, diskio slot, host, card handle, VFS path), and here the
        // handle is still ours to hand over.
        ESP_LOGW(TAG, "%s: releasing the card after the failed format", id_.c_str());
        esp_vfs_fat_sdcard_unmount(mountPoint_.c_str(), card_);
        card_ = nullptr;
        mounted_ = false;
        // `present_` is left as it is: the card did answer, it is the filesystem
        // that did not survive. The next mount attempt decides — and a card that
        // really is gone fails it and reports itself absent (see mountAttempts).
        return false;
    }

    // The re-mount after a successful mkfs can fail silently (see above), so ask
    // the volume for its size: only a mounted filesystem answers.
    uint64_t total = 0, free = 0;
    if (esp_vfs_fat_info(mountPoint_.c_str(), &total, &free) != ESP_OK) {
        error_ = "card did not come back after formatting";
        ESP_LOGE(TAG, "%s: %s", id_.c_str(), error_.c_str());
        // In this branch the helper has already released the card, the host and
        // the VFS path itself, so there is nothing to hand over — only a handle
        // that must not be used again.
        card_ = nullptr;
        mounted_ = false;
        return false;
    }

    error_.clear();
    ESP_LOGI(TAG, "microSD formatted (%s)", mountPoint_.c_str());
    return true;
}

#endif // CONFIG_IDF_TARGET_ESP32P4

} // namespace storage
} // namespace dhcp
