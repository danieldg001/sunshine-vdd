/**
 * @file src/platform/linux/virtual_display.h
 * @brief [Experimental] Linux virtual display via DRM EDID override.
 */
#pragma once

// standard includes
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

/**
 * @brief Virtual display backend that fakes a monitor by overriding the EDID of a disconnected DRM connector.
 */
namespace linux_vdisplay {

  /**
   * @brief Result of a virtual display operation.
   */
  enum class status_e {
    OK = 0,  ///< The operation succeeded.
    NOT_SUPPORTED = -1,  ///< debugfs or DRM devices are unavailable, or creation threw an exception.
    NO_CONNECTOR = -2,  ///< No disconnected connector is available to host the display.
    EDID_WRITE_FAILED = -3,  ///< The EDID could not be written, the mode or refresh rate was invalid, or the override could not be cleared.
    REPROBE_FAILED = -4,  ///< The connector status could not be forced on after the EDID was written, or restored on removal.
    COMPOSITOR_TIMEOUT = -5,  ///< The compositor did not pick up the new display in time.
  };

  /**
   * @brief Smallest width and height, in pixels, a virtual display is created with.
   *
   * DRM cursor planes are up to 256 px, and a mode smaller than the cursor is of no use for streaming.
   */
  constexpr int min_virtual_display_dimension = 256;

  /**
   * @brief Largest width and height, in pixels, a virtual display is created with.
   *
   * The EDID detailed timing descriptor stores the active width and height in 12 bits each.
   */
  constexpr int max_virtual_display_dimension = (1 << 12) - 1;

  /**
   * @brief A DRM connector that can host a virtual display.
   */
  struct connector_info_t {
    std::string card_path;  ///< Path of the DRM device node, for example `/dev/dri/card1`.
    int card_index;  ///< Index of the DRM card.
    std::uint32_t connector_id;  ///< DRM connector object id.
    std::uint32_t connector_type;  ///< DRM connector type, one of the `DRM_MODE_CONNECTOR_*` values.
    std::uint32_t connector_index;  ///< Per-type connector index, the `2` in `DP-2`.
    std::string sysfs_name;  ///< Connector name as in sysfs, for example `card1-DP-2`.
    std::string debugfs_edid_path;  ///< debugfs `edid_override` file of the connector.
    std::string sysfs_status_path;  ///< sysfs `status` file of the connector.
    std::string debugfs_hotplug_path;  ///< debugfs `trigger_hotplug` file of the connector, empty if unavailable.
  };

  /**
   * @brief Check whether a virtual display can be created on this system.
   * @return `true` if debugfs DRI is reachable through the helper and a DRM card can be opened.
   */
  bool is_supported();

  /**
   * @brief Undo an EDID override left behind by a previous run that did not shut down cleanly.
   *
   * The recovery state file is kept if the override cannot be cleared or the connector status cannot be restored,
   * so the next start retries.
   */
  void recover_crash_state();

  /**
   * @brief Replace the path of the recovery state file, so tests never touch the real appdata directory.
   * @param path New path; empty restores the default `vdisplay.state` in the appdata directory.
   * @return The previous override, empty if the default was in use.
   */
  std::filesystem::path set_state_file_path(std::filesystem::path path);

  /**
   * @brief Check whether a virtual display can be created with a mode.
   * @param width Width in pixels.
   * @param height Height in pixels.
   * @return `true` if both are between `min_virtual_display_dimension` and `max_virtual_display_dimension`.
   */
  bool is_supported_mode(int width, int height);

  /**
   * @brief Create a virtual display on a disconnected connector.
   *
   * The recovery state is persisted before the EDID is written. If the previous display could not
   * be removed, its status is returned and nothing new is created.
   * A mode or refresh rate that is not supported is rejected first, so the active display is left alone.
   * If creation throws, the display is kept as active so `remove()` can undo whatever was applied.
   * If the compositor does not give the connector a CRTC in time, `status_e::COMPOSITOR_TIMEOUT` is returned and the
   * display is kept as active for `remove()` as well.
   *
   * @param out_display_name Receives the connector name on success.
   * @param width Width in pixels.
   * @param height Height in pixels.
   * @param fps_millihertz Refresh rate in millihertz, clamped to 24-360 Hz.
   * @param client_id Client identifier used to derive a stable EDID serial, may be empty.
   * @return `status_e::OK` on success, otherwise the reason it failed.
   */
  status_e create(std::string &out_display_name, int width, int height, int fps_millihertz, const std::string &client_id);

  /**
   * @brief Remove the active virtual display, if any.
   * @return `status_e::OK` when nothing is active or the display was removed. `status_e::EDID_WRITE_FAILED`
   *         if the EDID override could not be cleared, `status_e::REPROBE_FAILED` if the connector status could not
   *         be restored; in both cases the display stays active so the call can be retried.
   */
  status_e remove();

  /**
   * @brief Check whether a virtual display is currently active.
   * @return `true` if a virtual display is active.
   */
  bool is_active();

  /**
   * @brief Get the KMS monitor index of the active virtual display, as kmsgrab numbers them.
   * @return The index, or -1 if no display is active or it has no CRTC yet.
   */
  int get_kms_index();

  /**
   * @brief Build the two-block EDID (base block plus CTA-861 extension) for a virtual display.
   *
   * Exposed so unit tests can check the generated bytes without touching hardware.
   *
   * @param width Width in pixels, the encoded preferred mode rounds it down to a multiple of 8.
   * @param height Height in pixels.
   * @param refresh_hz Refresh rate in Hz.
   * @param client_id Client identifier hashed into the EDID serial. Empty selects a fixed serial.
   * @return The 256 bytes of EDID.
   */
  std::array<std::uint8_t, 256> generate_edid(int width, int height, int refresh_hz, const std::string &client_id);

  /**
   * @brief Map a connector name to the output name kmsgrab matches against `output_name`.
   * @param connector Connector name, for example `card1-DP-2`.
   * @return The name without a leading `card<digits>-` prefix, for example `DP-2`; other input is returned unchanged.
   */
  std::string capture_name_for_connector(std::string_view connector);

  /**
   * @brief Convert the refresh rate a client asked for into the millihertz `create()` takes.
   * @param fps_hz Refresh rate in Hz, as sent by the client.
   * @return The rate clamped to the supported 24-360 Hz range, in millihertz.
   */
  int refresh_millihertz_for_fps(int fps_hz);

  /**
   * @brief Create the virtual display and point capture at it.
   *
   * On success the previous `config::video.output_name` is saved and replaced by the output of the new display.
   * On failure `end_capture()` runs, so no virtual display is left behind and capture stays on the configured output.
   *
   * @param width Width in pixels.
   * @param height Height in pixels.
   * @param fps_hz Refresh rate in Hz.
   * @param client_uuid Certificate UUID of the client, used to derive a stable EDID serial.
   * @return The status of `create()`.
   */
  status_e begin_capture(int width, int height, int fps_hz, const std::string &client_uuid);

  /**
   * @brief Remove the virtual display, if any, and restore the `output_name` saved by `begin_capture()`.
   */
  void end_capture();

}  // namespace linux_vdisplay
