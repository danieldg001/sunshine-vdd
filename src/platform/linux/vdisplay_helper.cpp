/**
 * @file src/platform/linux/vdisplay_helper.cpp
 * @brief Privileged helper for virtual display debugfs/sysfs writes.
 *
 * Installed with cap_dac_override+ep so the main sunshine process
 * does not need CAP_DAC_OVERRIDE in its capability set.
 */
#include "vdisplay_helper_path.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using linux_vdisplay::helper::hotplug_uevent;
using linux_vdisplay::helper::validate_path;
using linux_vdisplay::helper::validate_uevent_path;
using linux_vdisplay::helper::write_completed;

static int
  safe_open(const char *path) {
  return open(path, O_WRONLY | O_NOFOLLOW);
}

static int
  cmd_write_edid(const char *path) {
  uint8_t buf[256];
  ssize_t total = 0;
  while (total < static_cast<ssize_t>(sizeof(buf))) {
    ssize_t n = read(STDIN_FILENO, buf + total, sizeof(buf) - total);
    if (n <= 0) {
      break;
    }
    total += n;
  }
  if (total != 128 && total != 256) {
    fprintf(stderr, "Invalid EDID size: %zd\n", total);
    return 3;
  }
  int fd = safe_open(path);
  if (fd < 0) {
    perror("open");
    return 4;
  }
  ssize_t written = write(fd, buf, total);
  close(fd);
  if (written != total) {
    perror("write");
    return 5;
  }
  return 0;
}

static int
  cmd_clear_edid(const char *path) {
  int fd = safe_open(path);
  if (fd < 0) {
    perror("open");
    return 4;
  }
  ssize_t n = write(fd, "reset", 5);
  bool cleared = write_completed(n, 5);
  if (n < 0) {
    // Older kernels reject "reset"; an empty write clears the override instead.
    cleared = lseek(fd, 0, SEEK_SET) >= 0 && write_completed(write(fd, "", 0), 0);
  }
  close(fd);
  if (!cleared) {
    fprintf(stderr, "Failed to clear the EDID override\n");
    return 5;
  }
  return 0;
}

static int
  cmd_write_string(const char *path, const char *value) {
  int fd = safe_open(path);
  if (fd < 0) {
    perror("open");
    return 4;
  }
  ssize_t n = write(fd, value, strlen(value));
  close(fd);
  if (!write_completed(n, strlen(value))) {
    perror("write");
    return 5;
  }
  return 0;
}

/**
 * @brief Write a synthetic hotplug uevent to the sysfs `uevent` file of a DRM card.
 *
 * The event text is built here from a fresh random UUID; nothing the caller passes ends up in it.
 *
 * @param path `uevent` file, already checked by `validate_uevent_path()`.
 * @return 0 on success, 4 if a file cannot be opened, 5 if the UUID cannot be read or the write fails.
 */
static int
  cmd_trigger_drm_hotplug(const char *path) {
  char uuid[37] = {};
  int fd = open("/proc/sys/kernel/random/uuid", O_RDONLY);
  if (fd < 0) {
    perror("open");
    return 4;
  }
  ssize_t n = read(fd, uuid, 36);
  close(fd);
  auto event = n == 36 ? hotplug_uevent(uuid) : std::string {};
  if (event.empty()) {
    fprintf(stderr, "Could not read a random UUID\n");
    return 5;
  }
  return cmd_write_string(path, event.c_str());
}

int main(int argc, char *argv[]) {
  if (argc < 3) {
    fprintf(stderr, "Usage: %s <command> <path> [value]\n", argv[0]);
    return 1;
  }

  const char *command = argv[1];
  const char *path = argv[2];

  if (strcmp(command, "trigger-drm-hotplug") == 0) {
    if (!validate_uevent_path(path)) {
      fprintf(stderr, "Path rejected: %s\n", path);
      return 2;
    }
    return cmd_trigger_drm_hotplug(path);
  }

  if (!validate_path(path)) {
    fprintf(stderr, "Path rejected: %s\n", path);
    return 2;
  }

  if (strcmp(command, "write-edid") == 0) {
    return cmd_write_edid(path);
  }
  if (strcmp(command, "clear-edid") == 0) {
    return cmd_clear_edid(path);
  }
  if (strcmp(command, "write-string") == 0) {
    if (argc < 4) {
      fprintf(stderr, "write-string requires a value argument\n");
      return 1;
    }
    const char *value = argv[3];
    static const char *allowed_values[] = {"1", "on", "off", "detect"};
    bool valid = false;
    for (auto v : allowed_values) {
      if (strcmp(value, v) == 0) {
        valid = true;
        break;
      }
    }
    if (!valid) {
      fprintf(stderr, "Value rejected: %s\n", value);
      return 2;
    }
    return cmd_write_string(path, value);
  }
  if (strcmp(command, "check-path") == 0) {
    struct stat st;
    return stat(path, &st) == 0 ? 0 : 1;
  }

  fprintf(stderr, "Unknown command: %s\n", command);
  return 1;
}
