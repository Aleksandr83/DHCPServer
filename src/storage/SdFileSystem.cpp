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

#else  // CONFIG_IDF_TARGET_ESP32P4

bool SdFileSystem::tryMount(int busWidth)
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
    mcfg.format_if_mount_failed = false;   // formatting is an explicit action
    mcfg.max_files = kMaxFiles;
    mcfg.allocation_unit_size = kAllocUnit;
    mcfg.disk_status_check_enable = false;
    mcfg.use_one_fat = false;

    sdmmc_card_t* card = nullptr;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(mountPoint_.c_str(), &host, &slot,
                                            &mcfg, &card);
    if (err == ESP_OK) {
        card_ = card;
        mounted_ = true;
        present_ = true;
        error_.clear();
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
    if (tryMount(4)) return true;
    notes = attemptError_;
#endif
    if (tryMount(1)) return true;
    notes += notes.empty() ? attemptError_ : ("; " + attemptError_);

    error_ = notes;
    present_ = false;

    // Nothing answered with this switch polarity. Flip it for the *next* call
    // (the volume is retried every 5 s) instead of requiring a rebuild: a wrong
    // polarity leaves the card unpowered, and that is indistinguishable from a
    // dead card. The Waveshare board is active-low (P-MOSFET, gate pulled
    // down), so this normally never fires.
#if CONFIG_FILES_SD_PWR_GPIO >= 0
    pwrInverted_ = !pwrInverted_;
    ESP_LOGW(TAG, "%s: no answer — switching the slot power to the %s level on the next try",
             id_.c_str(), pwrInverted_ ? "opposite" : "configured");
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
    if (!mounted_) {
        error_ = "card is not mounted";
        return false;
    }

    ESP_LOGW(TAG, "formatting the microSD card at %s — all data will be lost",
             mountPoint_.c_str());
    esp_err_t err = esp_vfs_fat_sdcard_format(mountPoint_.c_str(), card_);
    if (err != ESP_OK) {
        error_ = std::string("format failed: ") + esp_err_to_name(err);
        ESP_LOGE(TAG, "%s: %s", id_.c_str(), error_.c_str());
        return false;
    }

    error_.clear();
    ESP_LOGI(TAG, "microSD formatted (%s)", mountPoint_.c_str());
    return true;
}

#endif // CONFIG_IDF_TARGET_ESP32P4

} // namespace storage
} // namespace dhcp
