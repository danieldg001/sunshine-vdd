/**
 * @file src/platform/linux/misc.h
 * @brief Miscellaneous declarations for Linux.
 */
#pragma once

// standard includes
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <thread>
#include <unistd.h>
#include <vector>

// local includes
#include "src/utility.h"

#ifndef DOXYGEN
KITTY_USING_MOVE_T(file_t, int, -1, {
  if (el >= 0) {
    close(el);
  }
});
#else
/**
 * @brief Move-only wrapper for a POSIX file descriptor.
 */
class file_t;
#endif

/**
 * @brief Enumerates supported window system options.
 */
enum class window_system_e {
  NONE,  ///< No window system
  X11,  ///< X11
  WAYLAND,  ///< Wayland
};

extern window_system_e window_system;  ///< Window system.

namespace dyn {
  /**
   * @brief Generic GLX procedure pointer returned by the loader.
   */
  typedef void (*apiproc)(void);

  int load(void *handle, const std::vector<std::tuple<apiproc *, const char *>> &funcs, bool strict = true);
  void *handle(const std::vector<const char *> &libs);

}  // namespace dyn

namespace platf {
  /**
   * @brief Remove environment variables that can load untrusted GUI modules.
   *
   * Loader paths and module lists used by Qt, GTK, GIO, and GdkPixbuf are removed before any of those libraries
   * can initialize. The caller determines whether privileged execution requires the policy.
   *
   * @return `true` when the environment is safe to use; `false` if a variable could not be removed.
   */
  bool sanitize_process_environment();

  /**
   * @brief Open a DRM card node and drop implicit DRM master, if any.
   *
   * Performs `open(path, flags | O_CLOEXEC)` and probes the resulting fd with
   * `DRM_IOCTL_AUTH_MAGIC`. If the kernel implicitly handed us master, calls
   * `drmDropMaster` and re-verifies before returning. Master check/drop failures
   * are logged as warnings but do not fail the call: the caller still receives
   * a usable fd.
   *
   * Callers should use this helper for any `/dev/dri/cardN` open so we never
   * keep implicit master and block compositors from re-acquiring it on VT
   * switches.
   *
   * @param path Path to the DRM card node (e.g. `/dev/dri/card0`).
   * @param flags `open()` flags. `O_CLOEXEC` is always OR-ed in.
   * @return A file descriptor on success, or `-1` if `open()` itself fails.
   */
  int open_drm_card_fd(const std::filesystem::path &path, int flags = O_RDWR);

  /**
   * @brief Generic frame pacing logic for Linux capture methods.
   *
   * Advances the frame pacing timeline and re-anchors it if the capture thread falls behind.
   *
   * @param next_frame Time point that the next frame will be targeted against.
   * @param delay Delay interval used to pace next frame.
   * @param logger The sleep_overshoot_logger for tracking discontinuities.
   */
  inline void handle_pacing(std::chrono::steady_clock::time_point &next_frame, std::chrono::nanoseconds delay, auto &logger) {
    auto now = std::chrono::steady_clock::now();

    if (next_frame > now) {
      std::this_thread::sleep_until(next_frame);
      logger.first_point(next_frame);
      logger.second_point_now_and_log();
    }

    next_frame += delay;
    if (next_frame < now) {  // some major slowdown happened; we couldn't keep up
      next_frame = now + delay;
    }
  }

#if defined(__linux__) && defined(SUNSHINE_BUILD_DRM)
  namespace kms {
    /**
     * @brief Time between two probes of the captured planes while no new frame is pending.
     *
     * Half a millisecond adds at most 6% of a 120 Hz frame of latency, and on a still screen it
     * costs about 2000 probes of two ioctls each per second.
     */
    inline constexpr std::chrono::microseconds frame_probe_interval {500};

    /**
     * @brief How much sooner than one frame interval after the previous capture a new frame may be captured.
     *
     * Covers the probe phase (one probe interval) plus the compositor's commit jitter, so a display
     * that flips at the stream rate is never held back.
     */
    inline constexpr std::chrono::microseconds frame_capture_tolerance {1000};

    /**
     * @brief How much less than one frame interval the capture may run ahead of the stream rate.
     *
     * Running ahead lets the capture catch up after the first frame that follows a still screen
     * lands just after a capture; a whole frame would allow two captures more than the stream rate
     * in some one-second windows, because `video::capture_frame_interval()` truncates the interval
     * to whole nanoseconds. One microsecond covers that truncation at any rate below 1000 fps and
     * keeps every one-second window to at most one capture more than the stream rate.
     */
    inline constexpr std::chrono::microseconds frame_catch_up_margin {1};

    /**
     * @brief Delay, in half frame intervals, from the last new frame to the first repeat of an unchanged frame.
     *
     * On a still screen the encoder must keep receiving frames, as it did with the fixed timer.
     * Reporting "no new frame" instead would leave it to its own `minimum_fps_target` timeout, which
     * by default is two frame intervals and lands just before each flip of 60 Hz content at a
     * 120 fps stream. Repeating one frame interval after the last new frame would land on the next
     * flip of a display that runs at the stream rate. One and a half intervals, then one interval
     * per repeat, keeps every repeat half a frame away from any flip on the display's frame grid.
     */
    inline constexpr int frame_repeat_delay_half_frames = 3;

    /**
     * @brief What the KMS capture loop does after probing the captured planes.
     */
    enum class frame_action_e {
      wait,  ///< Sleep until `frame_wait_deadline()` and probe again.
      capture,  ///< Capture the new frame now.
      repeat,  ///< Capture the unchanged frame again to keep the encoder fed.
    };

    /**
     * @brief Schedule kept by the KMS capture loop between two probes.
     *
     * A default-constructed schedule lets the first probe capture at once.
     */
    struct frame_pacing_t {
      std::chrono::steady_clock::time_point next_capture;  ///< Earliest time a new frame may be captured.
      std::chrono::steady_clock::time_point next_repeat;  ///< Time the unchanged frame is captured again.
      std::chrono::steady_clock::time_point rate_slot;  ///< Stream-rate slot of the last capture or repeat, which bounds the long-run rate.
    };

    /**
     * @brief Action chosen by `pace_frame()` and the schedule to keep once that action succeeds.
     */
    struct frame_decision_t {
      frame_action_e action;  ///< What the capture loop does now.
      frame_pacing_t pacing;  ///< Schedule to keep after a successful capture or repeat; unchanged for `wait`.
    };

    /**
     * @brief Decide whether to capture, repeat or wait.
     *
     * A new frame is a different framebuffer on the primary plane or a cursor plane change. It is
     * captured as soon as two limits allow:
     * - one frame interval minus `frame_capture_tolerance` since the previous capture, or half a
     *   frame interval minus the tolerance since a repeat, which sits half a frame before the next
     *   flip on the display's frame grid;
     * - the rate budget: captures and repeats together may run at most one frame interval minus
     *   `frame_catch_up_margin` ahead of the stream rate. That allowance lets the capture catch up, a millisecond
     *   per frame, when the first frame after a still screen lands just after a capture; without it
     *   the capture would trail the display by up to a frame for good. It also bounds every window
     *   of one second to at most one capture more than the stream rate, and keeps the long-run rate
     *   at the stream rate.
     * When nothing changes, the frame is repeated `frame_repeat_delay_half_frames` half intervals
     * after the last new frame, and then every frame interval.
     *
     * @param pacing Current schedule.
     * @param captured_fb_id Framebuffer of the last capture.
     * @param current_fb_id Framebuffer on the primary plane now.
     * @param cursor_changed Whether the cursor plane changed since the last capture.
     * @param now Current time.
     * @param frame_interval Interval between two frames at the stream rate.
     * @return Action for the capture loop, and the schedule to commit if that action succeeds. A capture
     *         or repeat that fails leaves the current schedule as it is, so the next probe decides again
     *         from the same state.
     */
    inline frame_decision_t pace_frame(const frame_pacing_t &pacing, std::uint32_t captured_fb_id, std::uint32_t current_fb_id, bool cursor_changed, std::chrono::steady_clock::time_point now, std::chrono::nanoseconds frame_interval) {
      const bool changed = current_fb_id != captured_fb_id || cursor_changed;
      if (now < pacing.next_capture || (!changed && now < pacing.next_repeat)) {
        return {frame_action_e::wait, pacing};
      }

      frame_pacing_t next = pacing;
      next.rate_slot = std::max(pacing.rate_slot + frame_interval, now);
      if (changed) {
        next.next_capture = std::max(next.rate_slot + frame_catch_up_margin, now + frame_interval - frame_capture_tolerance);
        next.next_repeat = now + frame_interval * frame_repeat_delay_half_frames / 2;
        return {frame_action_e::capture, next};
      }

      next.next_capture = std::max(next.rate_slot + frame_catch_up_margin, now + frame_interval / 2 - frame_capture_tolerance);
      next.next_repeat = pacing.next_repeat + frame_interval;
      if (next.next_repeat <= now) {
        next.next_repeat = now + frame_interval;
      }
      return {frame_action_e::repeat, next};
    }

    /**
     * @brief Time at which the capture loop probes again after `frame_action_e::wait`.
     *
     * @param pacing Current schedule.
     * @param now Current time.
     * @return The start of the next capture window, or one probe interval from now when it is already open.
     */
    inline std::chrono::steady_clock::time_point frame_wait_deadline(const frame_pacing_t &pacing, std::chrono::steady_clock::time_point now) {
      return now < pacing.next_capture ? pacing.next_capture : now + frame_probe_interval;
    }

    /**
     * @brief Force-construct the DRM privileged worker thread while the calling
     *        thread still holds CAP_SYS_ADMIN in CAP_PERMITTED.
     *
     * This must be called before dropping CAP_SYS_ADMIN from the calling thread,
     * since newly-created threads inherit its capability sets.
     */
    void ensure_privileged_drm_worker_started();

    /**
     * @brief Drop the DRM worker thread's privileges.
     */
    void drop_drm_worker_privileges();

    /**
     * @brief Open a DRM card file descriptor using the privileged DRM worker.
     *
     * @param path Path to the DRM card node.
     * @return A file descriptor on success, or `-1` on failure.
     */
    int privileged_open_drm_card_fd(const char *path);
  }  // namespace kms
#endif
}  // namespace platf
