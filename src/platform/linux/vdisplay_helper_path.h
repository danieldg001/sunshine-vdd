/**
 * @file src/platform/linux/vdisplay_helper_path.h
 * @brief Pure checks shared by the privileged virtual display helper and its tests.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstring>
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
      regex_t re;
      if (regcomp(&re, pattern, REG_EXTENDED | REG_NOSUB) != 0) {
        continue;
      }
      int match = regexec(&re, path, 0, nullptr, 0);
      regfree(&re);
      if (match == 0) {
        return true;
      }
    }

    return false;
  }

}  // namespace linux_vdisplay::helper
