/**
 * @file src/platform/linux/vdisplay_helper_path.h
 * @brief Pure checks shared by the privileged virtual display helper and its tests.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstring>
#include <string>
#include <sys/types.h>

// platform includes
#include <regex.h>

namespace linux_vdisplay::helper {

  /**
   * @brief Decide whether a write() call wrote everything it was asked to.
   *
   * A negative result is an error and a short count is a partial write; neither counts as success.
   *
   * @param written Return value of write().
   * @param expected Number of bytes the call was asked to write.
   * @return `true` only if exactly `expected` bytes were written.
   */
  inline bool write_completed(ssize_t written, std::size_t expected) {
    return written >= 0 && static_cast<std::size_t>(written) == expected;
  }

  /**
   * @brief Match a whole string against an anchored POSIX extended regular expression.
   * @param pattern Pattern, anchored with `^` and `$`.
   * @param text NUL-terminated string to check.
   * @return `true` if the pattern compiles and matches.
   */
  inline bool matches(const char *pattern, const char *text) {
    regex_t re;
    if (regcomp(&re, pattern, REG_EXTENDED | REG_NOSUB) != 0) {
      return false;
    }
    int match = regexec(&re, text, 0, nullptr, 0);
    regfree(&re);
    return match == 0;
  }

  /**
   * @brief Check whether the privileged helper is allowed to touch a path.
   *
   * Only the debugfs DRI root, the per-connector `edid_override` and `trigger_hotplug`
   * files, and the sysfs connector `status` file are accepted. The match is anchored on
   * both ends, so paths containing `..` or any other component are rejected.
   *
   * @param path NUL-terminated path to check.
   * @return `true` if the path is on the allow-list.
   */
  inline bool validate_path(const char *path) {
    if (strcmp(path, "/sys/kernel/debug/dri") == 0) {
      return true;
    }

    static const char *patterns[] = {
      "^/sys/kernel/debug/dri/[0-9]+/[A-Za-z]+(-[A-Za-z]+)*-[0-9]+/edid_override$",
      "^/sys/kernel/debug/dri/[0-9]+/[A-Za-z]+(-[A-Za-z]+)*-[0-9]+/trigger_hotplug$",
      "^/sys/class/drm/card[0-9]+-[A-Za-z]+(-[A-Za-z]+)*-[0-9]+/status$",
    };

    for (auto pattern : patterns) {
      if (matches(pattern, path)) {
        return true;
      }
    }

    return false;
  }

  /**
   * @brief Check whether a path is the sysfs `uevent` file of a DRM card, the only file `trigger-drm-hotplug` writes.
   *
   * It is kept off the `validate_path()` allow-list so no other command can write to it.
   *
   * @param path NUL-terminated path to check.
   * @return `true` for `/sys/class/drm/card<N>/uevent`.
   */
  inline bool validate_uevent_path(const char *path) {
    return matches("^/sys/class/drm/card[0-9]+/uevent$", path);
  }

  /**
   * @brief Build the synthetic uevent that announces a hotplug on a DRM card.
   *
   * The kernel emits it as a `change` event of the card, which compositors handle as a hotplug and answer by
   * reprobing the connectors. It stores the extra key as `SYNTH_ARG_HOTPLUG=1`, not as `HOTPLUG=1`.
   *
   * @param uuid Lowercase canonical UUID, 36 characters, as read from `/proc/sys/kernel/random/uuid`.
   * @return `change <uuid> HOTPLUG=1`, or an empty string if `uuid` is anything else.
   */
  inline std::string hotplug_uevent(const char *uuid) {
    if (!matches("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$", uuid)) {
      return {};
    }
    return std::string {"change "} + uuid + " HOTPLUG=1";
  }

}  // namespace linux_vdisplay::helper
