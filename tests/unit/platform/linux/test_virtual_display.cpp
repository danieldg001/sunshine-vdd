/**
 * @file tests/unit/platform/linux/test_virtual_display.cpp
 * @brief Tests for the Linux virtual display EDID generation, helper path allow-list, hotplug uevent, capture name and crash recovery.
 */

#ifdef SUNSHINE_BUILD_DRM

  // test includes
  #include "../../../tests_common.h"

  // standard includes
  #include <array>
  #include <cstdint>
  #include <cstdlib>
  #include <filesystem>
  #include <fstream>
  #include <limits>
  #include <numeric>
  #include <string>
  #include <unistd.h>
  #include <vector>

  // local includes
  #include "src/platform/linux/vdisplay_helper_path.h"
  #include "src/platform/linux/virtual_display.h"

namespace {

  using edid_t = std::array<std::uint8_t, 256>;

  struct mode_spec_t {
    int width;
    int height;
    int refresh_hz;
  };

  struct dtd_t {
    int pixel_clock_khz;
    int h_active;
    int h_blanking;
    int v_active;
    int v_blanking;
  };

  constexpr mode_spec_t modes[] = {
    {2732, 2048, 60},
    {1920, 1080, 60},
    {1280, 720, 24},
    {3840, 2160, 60},
    {2560, 1440, 144},
  };

  /**
   * @brief Sum a range of bytes modulo 256.
   */
  std::uint8_t block_sum(const edid_t &edid, std::size_t first, std::size_t count) {
    return static_cast<std::uint8_t>(std::accumulate(edid.begin() + first, edid.begin() + first + count, 0));
  }

  /**
   * @brief Decode a detailed timing descriptor as laid out by the EDID 1.4 specification.
   */
  dtd_t decode_dtd(const edid_t &edid, std::size_t offset) {
    const auto *d = &edid[offset];
    dtd_t t {};
    t.pixel_clock_khz = (d[0] | (d[1] << 8)) * 10;
    t.h_active = d[2] | ((d[4] >> 4) << 8);
    t.h_blanking = d[3] | ((d[4] & 0x0F) << 8);
    t.v_active = d[5] | ((d[7] >> 4) << 8);
    t.v_blanking = d[6] | ((d[7] & 0x0F) << 8);
    return t;
  }

  std::uint32_t decode_serial(const edid_t &edid) {
    return edid[12] | (edid[13] << 8) | (edid[14] << 16) | (static_cast<std::uint32_t>(edid[15]) << 24);
  }

  edid_t make(const mode_spec_t &m, const std::string &client_id = "client") {
    return linux_vdisplay::generate_edid(m.width, m.height, m.refresh_hz, client_id);
  }
}  // namespace

TEST(VirtualDisplayEdid, BlocksAreWholeAndChecksumToZero) {
  for (const auto &m : modes) {
    SCOPED_TRACE(std::to_string(m.width) + "x" + std::to_string(m.height) + "@" + std::to_string(m.refresh_hz));
    auto edid = make(m);
    ASSERT_EQ(edid.size() % 128, 0u);
    for (std::size_t block = 0; block < edid.size() / 128; ++block) {
      EXPECT_EQ(block_sum(edid, block * 128, 128), 0) << "block " << block;
    }
  }
}

TEST(VirtualDisplayEdid, HeaderVersionAndExtensionCountAreValid) {
  auto edid = make(modes[0]);
  const std::array<std::uint8_t, 8> header {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
  EXPECT_TRUE(std::equal(header.begin(), header.end(), edid.begin()));
  EXPECT_EQ(edid[18], 1);
  EXPECT_EQ(edid[19], 4);
  EXPECT_EQ(edid[126], edid.size() / 128 - 1);
  EXPECT_EQ(edid[128], 0x02) << "CTA-861 extension tag";
}

TEST(VirtualDisplayEdid, PreferredModeRoundsWidthDownToMultipleOfEight) {
  auto edid = make({2732, 2048, 60});
  auto preferred = decode_dtd(edid, 54);
  EXPECT_EQ(preferred.h_active, 2728);
  EXPECT_EQ(preferred.v_active, 2048);
}

TEST(VirtualDisplayEdid, PreferredModeKeepsAlignedWidth) {
  auto edid = make({1920, 1080, 60});
  auto preferred = decode_dtd(edid, 54);
  EXPECT_EQ(preferred.h_active, 1920);
  EXPECT_EQ(preferred.v_active, 1080);
}

TEST(VirtualDisplayEdid, PreferredModeTimingMatchesRequestedRefresh) {
  for (const auto &m : modes) {
    SCOPED_TRACE(std::to_string(m.width) + "x" + std::to_string(m.height) + "@" + std::to_string(m.refresh_hz));
    auto edid = make(m);
    auto t = decode_dtd(edid, 54);
    ASSERT_GT(t.pixel_clock_khz, 0);
    double refresh = t.pixel_clock_khz * 1000.0 / ((t.h_active + t.h_blanking) * (t.v_active + t.v_blanking));
    EXPECT_NEAR(refresh, m.refresh_hz, 0.5);
  }
}

TEST(VirtualDisplayEdid, PreferredModeNeverExceedsRequestedRefreshWhenPixelClockOverflows) {
  // The DTD pixel clock is 16 bits in units of 10 kHz (at most 655.35 MHz), so 4K at 240 Hz cannot be encoded as asked.
  auto edid = make({3840, 2160, 240});
  auto t = decode_dtd(edid, 54);
  ASSERT_GT(t.pixel_clock_khz, 0);
  double refresh = t.pixel_clock_khz * 1000.0 / ((t.h_active + t.h_blanking) * (t.v_active + t.v_blanking));
  EXPECT_GT(refresh, 0.0);
  EXPECT_LE(refresh, 240.0);
  for (std::size_t block = 0; block < edid.size() / 128; ++block) {
    EXPECT_EQ(block_sum(edid, block * 128, 128), 0) << "block " << block;
  }
}

TEST(VirtualDisplayEdid, SerialIsStablePerClientAndDiffersBetweenClients) {
  const mode_spec_t mode {2732, 2048, 60};
  auto first = decode_serial(make(mode, "0123ABCD-0000-0000-0000-000000000000"));
  auto again = decode_serial(make(mode, "0123ABCD-0000-0000-0000-000000000000"));
  auto other = decode_serial(make(mode, "89EFCDAB-0000-0000-0000-000000000000"));
  EXPECT_EQ(first, again);
  EXPECT_NE(first, other);
}

TEST(VirtualDisplayEdid, SerialDoesNotDependOnMode) {
  auto a = decode_serial(make({2732, 2048, 60}, "client"));
  auto b = decode_serial(make({1920, 1080, 120}, "client"));
  EXPECT_EQ(a, b);
}

TEST(VirtualDisplayHelperWrite, OnlyAFullWriteCounts) {
  using linux_vdisplay::helper::write_completed;
  EXPECT_TRUE(write_completed(5, 5));
  EXPECT_TRUE(write_completed(0, 0));
  for (ssize_t partial : {0, 1, 4}) {
    EXPECT_FALSE(write_completed(partial, 5)) << partial;
  }
  EXPECT_FALSE(write_completed(6, 5));
  EXPECT_FALSE(write_completed(-1, 5));
  EXPECT_FALSE(write_completed(-1, 0));
  EXPECT_FALSE(write_completed(1, 0));
}

TEST(VirtualDisplayHelperPath, AcceptsAllowListedPaths) {
  using linux_vdisplay::helper::validate_path;
  EXPECT_TRUE(validate_path("/sys/kernel/debug/dri"));
  EXPECT_TRUE(validate_path("/sys/kernel/debug/dri/1/DP-2/edid_override"));
  EXPECT_TRUE(validate_path("/sys/kernel/debug/dri/0/HDMI-A-1/edid_override"));
  EXPECT_TRUE(validate_path("/sys/kernel/debug/dri/1/DP-2/trigger_hotplug"));
  EXPECT_TRUE(validate_path("/sys/class/drm/card1-DP-2/status"));
  EXPECT_TRUE(validate_path("/sys/class/drm/card0-HDMI-A-1/status"));
}

TEST(VirtualDisplayHelperPath, RejectsPathsOutsideAllowedPrefixes) {
  using linux_vdisplay::helper::validate_path;
  for (const char *path : {
         "",
         "/",
         "/etc/passwd",
         "/sys/kernel/debug",
         "/sys/kernel/debug/dri/",
         "/sys/kernel/debug/dri/1/DP-2/state",
         "/sys/kernel/debug/dri/1/DP-2/edid_override/extra",
         "/sys/kernel/debug/driver/1/DP-2/edid_override",
         "/tmp/sys/kernel/debug/dri/1/DP-2/edid_override",
         "/sys/class/drm/card1-DP-2/enabled",
         "/sys/class/drm/card1/status",
         "sys/class/drm/card1-DP-2/status",
       }) {
    EXPECT_FALSE(validate_path(path)) << path;
  }
}

TEST(VirtualDisplayHelperPath, RejectsParentDirectoryComponents) {
  using linux_vdisplay::helper::validate_path;
  for (const char *path : {
         "/sys/kernel/debug/dri/1/DP-2/../../../../../etc/shadow",
         "/sys/kernel/debug/dri/../../../../etc/shadow",
         "/sys/kernel/debug/dri/1/DP-2/edid_override/../../../../../../etc/shadow",
         "/sys/kernel/debug/dri/1/../1/DP-2/edid_override",
         "/sys/class/drm/card1-DP-2/../../../../etc/shadow",
         "/sys/class/drm/card1-DP-2/status/..",
         "/sys/kernel/debug/dri/..",
       }) {
    EXPECT_FALSE(validate_path(path)) << path;
  }
}

TEST(VirtualDisplayHelperUevent, AcceptsOnlyTheUeventFileOfADrmCard) {
  using linux_vdisplay::helper::validate_uevent_path;
  EXPECT_TRUE(validate_uevent_path("/sys/class/drm/card1/uevent"));
  EXPECT_TRUE(validate_uevent_path("/sys/class/drm/card10/uevent"));
  for (const char *path : {
         "",
         "/sys/class/drm/card1",
         "/sys/class/drm/card1/",
         "/sys/class/drm/card/uevent",
         "/sys/class/drm/card1-DP-2/uevent",
         "/sys/class/drm/renderD128/uevent",
         "/sys/class/drm/card1/status",
         "/sys/class/drm/card1/uevent/..",
         "/sys/class/drm/card1/../card2/uevent",
         "/sys/devices/pci0000:00/0000:00:03.1/0000:07:00.0/drm/card1/uevent",
         "sys/class/drm/card1/uevent",
       }) {
    EXPECT_FALSE(validate_uevent_path(path)) << path;
  }
}

TEST(VirtualDisplayHelperUevent, CardUeventFileIsNotOnTheGeneralAllowList) {
  EXPECT_FALSE(linux_vdisplay::helper::validate_path("/sys/class/drm/card1/uevent"));
}

TEST(VirtualDisplayHelperUevent, BuildsAChangeEventAroundTheUuid) {
  EXPECT_EQ(linux_vdisplay::helper::hotplug_uevent("a3440b2b-c07d-4881-b8f4-54ea9e22c325"), "change a3440b2b-c07d-4881-b8f4-54ea9e22c325 HOTPLUG=1");
}

TEST(VirtualDisplayHelperUevent, RefusesAnythingButALowercaseCanonicalUuid) {
  using linux_vdisplay::helper::hotplug_uevent;
  for (const char *uuid : {
         "",
         "not-a-uuid",
         "a3440b2b-c07d-4881-b8f4-54ea9e22c32",
         "a3440b2b-c07d-4881-b8f4-54ea9e22c3255",
         "a3440b2bc07d4881b8f454ea9e22c325",
         "A3440B2B-C07D-4881-B8F4-54EA9E22C325",
         "a3440b2b-c07d-4881-b8f4-54ea9e22c325\n",
         "a3440b2b-c07d-4881-b8f4-54ea9e22c325 HOTPLUG=0",
       }) {
    EXPECT_EQ(hotplug_uevent(uuid), "") << uuid;
  }
}

TEST(VirtualDisplayCaptureName, StripsCardPrefix) {
  using linux_vdisplay::capture_name_for_connector;
  EXPECT_EQ(capture_name_for_connector("card1-DP-2"), "DP-2");
  EXPECT_EQ(capture_name_for_connector("card10-HDMI-A-1"), "HDMI-A-1");
  EXPECT_EQ(capture_name_for_connector("DP-2"), "DP-2");
  EXPECT_EQ(capture_name_for_connector(""), "");
  EXPECT_EQ(capture_name_for_connector("cardX-DP-2"), "cardX-DP-2");
}

TEST(VirtualDisplayRefresh, ClampsClientFpsBeforeConvertingToMillihertz) {
  using linux_vdisplay::refresh_millihertz_for_fps;
  EXPECT_EQ(refresh_millihertz_for_fps(60), 60000);
  EXPECT_EQ(refresh_millihertz_for_fps(24), 24000);
  EXPECT_EQ(refresh_millihertz_for_fps(360), 360000);
  EXPECT_EQ(refresh_millihertz_for_fps(0), 24000);
  EXPECT_EQ(refresh_millihertz_for_fps(-5), 24000);
  EXPECT_EQ(refresh_millihertz_for_fps(std::numeric_limits<int>::min()), 24000);
  EXPECT_EQ(refresh_millihertz_for_fps(3000000), 360000);
  EXPECT_EQ(refresh_millihertz_for_fps(std::numeric_limits<int>::max()), 360000);
}

TEST(VirtualDisplayMode, AcceptsModesAtLeastTheMinimumSize) {
  using linux_vdisplay::is_supported_mode;
  EXPECT_TRUE(is_supported_mode(256, 256));
  EXPECT_TRUE(is_supported_mode(2732, 2048));
}

TEST(VirtualDisplayMode, RejectsModesBelowTheMinimumSizeOrNonPositive) {
  using linux_vdisplay::is_supported_mode;
  EXPECT_FALSE(is_supported_mode(255, 2048));
  EXPECT_FALSE(is_supported_mode(2732, 255));
  EXPECT_FALSE(is_supported_mode(0, 0));
  EXPECT_FALSE(is_supported_mode(-1, 100));
}

TEST(VirtualDisplayMode, AcceptsTheLargestModeTheEdidCanEncode) {
  using linux_vdisplay::is_supported_mode;
  EXPECT_TRUE(is_supported_mode(4095, 4095));
  EXPECT_TRUE(is_supported_mode(256, 4095));
  EXPECT_TRUE(is_supported_mode(4095, 256));
}

TEST(VirtualDisplayMode, RejectsModesTheEdidCannotEncode) {
  using linux_vdisplay::is_supported_mode;
  constexpr int int_max = std::numeric_limits<int>::max();
  EXPECT_FALSE(is_supported_mode(4096, 2048));
  EXPECT_FALSE(is_supported_mode(2732, 4096));
  EXPECT_FALSE(is_supported_mode(4096, 4096));
  EXPECT_FALSE(is_supported_mode(int_max, 2048));
  EXPECT_FALSE(is_supported_mode(2732, int_max));
  EXPECT_FALSE(is_supported_mode(int_max, int_max));
}

TEST(VirtualDisplayEdid, LargestSupportedModeIsEncodedExactly) {
  auto edid = make({4095, 4095, 60});
  auto preferred = decode_dtd(edid, 54);
  EXPECT_EQ(preferred.h_active, 4088);
  EXPECT_EQ(preferred.v_active, 4095);
  for (std::size_t block = 0; block < edid.size() / 128; ++block) {
    EXPECT_EQ(block_sum(edid, block * 128, 128), 0) << "block " << block;
  }
}

TEST(VirtualDisplayMode, CreateRejectsInvalidRequestBeforeTouchingTheSystem) {
  std::string name;
  EXPECT_EQ(linux_vdisplay::create(name, 100, 100, 60000, ""), linux_vdisplay::status_e::EDID_WRITE_FAILED);
  EXPECT_EQ(linux_vdisplay::create(name, 4096, 2048, 60000, ""), linux_vdisplay::status_e::EDID_WRITE_FAILED);
  EXPECT_EQ(linux_vdisplay::create(name, std::numeric_limits<int>::max(), 2048, 60000, ""), linux_vdisplay::status_e::EDID_WRITE_FAILED);
  EXPECT_EQ(linux_vdisplay::create(name, 1920, 1080, 0, ""), linux_vdisplay::status_e::EDID_WRITE_FAILED);
  EXPECT_TRUE(name.empty());
}

using namespace std::string_literals;

namespace {
  namespace fs = std::filesystem;

  /**
   * @brief Run `recover_crash_state()` against a fake `sunshine-vdisplay-helper` on `PATH` that records each call.
   *
   * Each test gets its own temporary directory, which also holds the recovery state file, so the real appdata
   * directory is never touched.
   */
  class VirtualDisplayRecovery: public ::testing::Test {
  protected:
    void SetUp() override {
      std::string dir_template = (fs::temp_directory_path() / "sunshine_vdd_recovery_XXXXXX").string();
      ASSERT_NE(mkdtemp(dir_template.data()), nullptr);
      dir = dir_template;
      calls = dir / "calls";
      fail_detect = dir / "fail-detect";
      no_debugfs_hotplug = dir / "no-debugfs-hotplug";
      state_file = dir / "vdisplay.state";
      const auto helper = dir / "sunshine-vdisplay-helper";
      std::ofstream(helper) << "#!/bin/sh\n"
                            << "echo \"$*\" >> '" << calls.string() << "'\n"
                            << "[ \"$1 $3\" = 'write-string detect' ] && [ -e '" << fail_detect.string() << "' ] && exit 1\n"
                            << "[ \"$1\" = check-path ] && [ -e '" << no_debugfs_hotplug.string() << "' ] && exit 1\n"
                            << "exit 0\n";
      fs::permissions(helper, fs::perms::owner_all);

      const char *path = std::getenv("PATH");
      old_path = path ? path : "";
      setenv("PATH", (dir.string() + ":" + old_path).c_str(), 1);

      old_state_file = linux_vdisplay::set_state_file_path(state_file);
      std::ofstream(state_file) << state;
    }

    void TearDown() override {
      if (dir.empty()) {
        return;
      }
      linux_vdisplay::set_state_file_path(old_state_file);
      setenv("PATH", old_path.c_str(), 1);
      fs::remove_all(dir);
    }

    /**
     * @brief Read the helper calls recorded so far, one per line.
     */
    std::vector<std::string> helper_calls() const {
      std::vector<std::string> lines;
      std::ifstream f {calls};
      for (std::string line; std::getline(f, line);) {
        lines.push_back(line);
      }
      return lines;
    }

    static constexpr auto edid_path = "/sys/kernel/debug/dri/99/DP-99/edid_override";
    static constexpr auto status_path = "/sys/class/drm/card99-DP-99/status";
    static constexpr auto hotplug_path = "/sys/kernel/debug/dri/99/DP-99/trigger_hotplug";
    const std::string state = std::string {edid_path} + "\n" + status_path + "\n" + hotplug_path + "\ncard99-DP-99\n";

    fs::path state_file;
    fs::path old_state_file;
    fs::path dir;
    fs::path calls;
    fs::path fail_detect;
    fs::path no_debugfs_hotplug;
    std::string old_path;
  };
}  // namespace

TEST_F(VirtualDisplayRecovery, KeepsStateWhenTheConnectorStatusCannotBeRestored) {
  std::ofstream {fail_detect};
  linux_vdisplay::recover_crash_state();
  EXPECT_TRUE(fs::exists(state_file));
  const auto lines = helper_calls();
  ASSERT_GE(lines.size(), 2u);
  EXPECT_EQ(lines[0], "clear-edid "s + edid_path);
  EXPECT_EQ(lines[1], "write-string "s + status_path + " detect");
}

TEST_F(VirtualDisplayRecovery, ClearsStateOnceTheOverrideAndStatusAreRestored) {
  linux_vdisplay::recover_crash_state();
  EXPECT_FALSE(fs::exists(state_file));
  const std::vector<std::string> expected {
    "clear-edid "s + edid_path,
    "write-string "s + status_path + " detect",
    "check-path "s + hotplug_path,
    "write-string "s + hotplug_path + " 1",
  };
  EXPECT_EQ(helper_calls(), expected);
}

TEST_F(VirtualDisplayRecovery, AnnouncesTheHotplugOnTheCardWhenDebugfsHasNoTriggerFile) {
  std::ofstream {no_debugfs_hotplug};
  linux_vdisplay::recover_crash_state();
  EXPECT_FALSE(fs::exists(state_file));
  const std::vector<std::string> expected {
    "clear-edid "s + edid_path,
    "write-string "s + status_path + " detect",
    "check-path "s + hotplug_path,
    "trigger-drm-hotplug /sys/class/drm/card99/uevent",
  };
  EXPECT_EQ(helper_calls(), expected);
}

#endif  // SUNSHINE_BUILD_DRM
