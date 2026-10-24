/**
 * @file tests/unit/platform/linux/test_kmsgrab.cpp
 * @brief Tests for the presentation-driven pacing of KMS capture.
 */

#ifdef SUNSHINE_BUILD_DRM

  // test includes
  #include "../../../tests_common.h"

  // standard includes
  #include <algorithm>
  #include <chrono>
  #include <cstddef>
  #include <cstdint>
  #include <map>
  #include <vector>

  // local includes
  #include "src/platform/linux/misc.h"

namespace {
  using namespace std::chrono_literals;
  using platf::kms::frame_action_e;
  using platf::kms::frame_pacing_t;
  using time_point_t = std::chrono::steady_clock::time_point;

  constexpr std::chrono::nanoseconds frame_interval_120 {1'000'000'000LL / 120};
  const time_point_t t0 = time_point_t {} + 1h;

  /**
   * @brief Apply a decision as the capture loop does after a successful snapshot.
   *
   * @param pacing Schedule to advance.
   * @param captured_fb Framebuffer of the last capture.
   * @param current_fb Framebuffer on the plane now.
   * @param cursor_changed Whether the cursor plane changed.
   * @param now Current time.
   * @return Action taken.
   */
  frame_action_e step(frame_pacing_t &pacing, std::uint32_t captured_fb, std::uint32_t current_fb, bool cursor_changed, time_point_t now) {
    auto decision = platf::kms::pace_frame(pacing, captured_fb, current_fb, cursor_changed, now, frame_interval_120);
    pacing = decision.pacing;
    return decision.action;
  }

  /**
   * @brief Pacing state right after the first capture of a 120 fps stream, at `t0`.
   *
   * @return Pacing state as left by the capture.
   */
  frame_pacing_t paced_after_capture_at_t0() {
    frame_pacing_t pacing {};
    EXPECT_EQ(step(pacing, 0, 1, false, t0), frame_action_e::capture);
    return pacing;
  }

  /**
   * @brief One action taken by the simulated capture loop.
   */
  struct simulated_action_t {
    frame_action_e action;  ///< Capture or repeat.
    time_point_t at;  ///< When the action was taken.
    long frame;  ///< Index of the display frame on screen at that time.
    std::chrono::nanoseconds latency;  ///< Time since that frame reached the plane.
  };

  /**
   * @brief Drive the capture loop against a display that flips about every `display_period`.
   *
   * Mirrors the KMS capture loop: probe, decide, then sleep until `frame_wait_deadline` or capture.
   * The display cycles through three framebuffers like a compositor swapchain, and each flip lands
   * up to `jitter` early or late, in a fixed pattern, like compositor commits.
   *
   * @param display_period Mean time between two display flips.
   * @param phase Time of the first flip.
   * @param duration Simulated time.
   * @param jitter Largest offset of a flip from its nominal time.
   * @return Every capture and repeat with the frame on screen.
   */
  std::vector<simulated_action_t> simulate(std::chrono::nanoseconds display_period, std::chrono::nanoseconds phase, std::chrono::nanoseconds duration, std::chrono::nanoseconds jitter = 0ns) {
    constexpr std::chrono::nanoseconds capture_cost {200us};
    auto flip_time = [&](long frame) {
      return t0 + phase + frame * display_period + jitter * ((frame * 7) % 9 - 4) / 4;
    };
    auto frame_at = [&](time_point_t t) {
      auto frame = static_cast<long>((t - t0 - phase) / display_period) - 1;
      while (flip_time(frame + 1) <= t) {
        ++frame;
      }
      return frame;
    };
    auto fb_of = [](long frame) {
      return static_cast<std::uint32_t>(1 + (frame + 3) % 3);
    };

    std::vector<simulated_action_t> actions;
    frame_pacing_t pacing {};
    std::uint32_t captured_fb = 0;
    for (auto now = t0; now < t0 + duration;) {
      auto frame = frame_at(now);
      auto decision = platf::kms::pace_frame(pacing, captured_fb, fb_of(frame), false, now, frame_interval_120);
      if (decision.action == frame_action_e::wait) {
        now = platf::kms::frame_wait_deadline(pacing, now);
        continue;
      }
      pacing = decision.pacing;
      captured_fb = fb_of(frame);
      actions.push_back({decision.action, now, frame, now - flip_time(frame)});
      now += capture_cost;
    }
    return actions;
  }

  /**
   * @brief Largest number of actions in any one-second window.
   *
   * @param actions Simulated actions, in time order.
   * @return Most actions that fall in `[t, t + 1 s)` for some `t`.
   */
  std::size_t busiest_second(const std::vector<simulated_action_t> &actions) {
    std::size_t busiest = 0;
    for (std::size_t first = 0, last = 0; first < actions.size(); ++first) {
      while (last < actions.size() && actions[last].at < actions[first].at + 1s) {
        ++last;
      }
      busiest = std::max(busiest, last - first);
    }
    return busiest;
  }

  /**
   * @brief Check that each frame shown during a simulation was captured exactly once and promptly.
   *
   * @param actions Simulated actions.
   * @param skip_captures Leading captures exempt from the latency check while the capture catches up.
   * @param max_latency Longest allowed time from a flip to its capture.
   */
  void expect_every_frame_once(const std::vector<simulated_action_t> &actions, std::size_t skip_captures, std::chrono::nanoseconds max_latency) {
    std::map<long, int> captures_per_frame;
    std::size_t captures = 0;
    for (const auto &action : actions) {
      if (action.action != frame_action_e::capture) {
        continue;
      }
      if (captures++ >= skip_captures) {
        EXPECT_LE(action.latency, max_latency) << "frame " << action.frame;
      }
      ++captures_per_frame[action.frame];
    }
    for (long frame = actions.front().frame; frame < actions.back().frame; ++frame) {
      EXPECT_EQ(captures_per_frame[frame], 1) << "frame " << frame;
    }
  }
}  // namespace

TEST(KmsFramePacing, CapturesNewFramebufferAfterMinimumInterval) {
  auto pacing = paced_after_capture_at_t0();
  auto minimum = frame_interval_120 - platf::kms::frame_capture_tolerance;

  EXPECT_EQ(step(pacing, 1, 2, false, t0 + minimum), frame_action_e::capture);
}

TEST(KmsFramePacing, WaitsForNewFramebufferBeforeMinimumInterval) {
  auto pacing = paced_after_capture_at_t0();
  auto minimum = frame_interval_120 - platf::kms::frame_capture_tolerance;

  EXPECT_EQ(step(pacing, 1, 2, false, t0 + minimum - 1ns), frame_action_e::wait);
  EXPECT_EQ(platf::kms::frame_wait_deadline(pacing, t0 + 1ms), t0 + minimum);
}

TEST(KmsFramePacing, DecisionDoesNotTouchTheCurrentSchedule) {
  const auto pacing = paced_after_capture_at_t0();
  auto minimum = frame_interval_120 - platf::kms::frame_capture_tolerance;

  auto first = platf::kms::pace_frame(pacing, 1, 2, false, t0 + minimum, frame_interval_120);
  auto second = platf::kms::pace_frame(pacing, 1, 2, false, t0 + minimum, frame_interval_120);
  EXPECT_EQ(first.action, frame_action_e::capture);
  EXPECT_EQ(second.action, frame_action_e::capture);
  EXPECT_EQ(first.pacing.next_capture, second.pacing.next_capture);
  EXPECT_GT(first.pacing.next_capture, pacing.next_capture);

  auto waiting = platf::kms::pace_frame(pacing, 1, 2, false, t0 + 1ms, frame_interval_120);
  EXPECT_EQ(waiting.action, frame_action_e::wait);
  EXPECT_EQ(waiting.pacing.next_capture, pacing.next_capture);
  EXPECT_EQ(waiting.pacing.next_repeat, pacing.next_repeat);
  EXPECT_EQ(waiting.pacing.rate_slot, pacing.rate_slot);
}

TEST(KmsFramePacing, UnchangedFramebufferWaitsThenRepeatsHalfAFrameOffTheFlipGrid) {
  auto pacing = paced_after_capture_at_t0();
  auto repeat_at = t0 + frame_interval_120 * platf::kms::frame_repeat_delay_half_frames / 2;

  EXPECT_EQ(platf::kms::frame_repeat_delay_half_frames, 3);
  EXPECT_EQ(step(pacing, 1, 1, false, t0 + frame_interval_120), frame_action_e::wait);
  EXPECT_EQ(platf::kms::frame_wait_deadline(pacing, t0 + frame_interval_120), t0 + frame_interval_120 + platf::kms::frame_probe_interval);
  EXPECT_EQ(step(pacing, 1, 1, false, repeat_at - 1ns), frame_action_e::wait);
  EXPECT_EQ(step(pacing, 1, 1, false, repeat_at), frame_action_e::repeat);
  EXPECT_EQ(step(pacing, 1, 1, false, repeat_at + frame_interval_120 - 1ns), frame_action_e::wait);
  EXPECT_EQ(step(pacing, 1, 1, false, repeat_at + frame_interval_120), frame_action_e::repeat);
}

TEST(KmsFramePacing, NewFramebufferRightAfterRepeatWaitsHalfAFrame) {
  auto pacing = paced_after_capture_at_t0();
  auto repeat_at = t0 + frame_interval_120 * platf::kms::frame_repeat_delay_half_frames / 2;
  ASSERT_EQ(step(pacing, 1, 1, false, repeat_at), frame_action_e::repeat);

  auto after_repeat = frame_interval_120 / 2 - platf::kms::frame_capture_tolerance;
  EXPECT_EQ(step(pacing, 1, 2, false, repeat_at + 500us), frame_action_e::wait);
  EXPECT_EQ(platf::kms::frame_wait_deadline(pacing, repeat_at + 500us), repeat_at + after_repeat);
  EXPECT_EQ(step(pacing, 1, 2, false, repeat_at + after_repeat), frame_action_e::capture);
}

TEST(KmsFramePacing, CursorOnlyChangeCaptures) {
  auto pacing = paced_after_capture_at_t0();

  EXPECT_EQ(step(pacing, 1, 1, true, t0 + frame_interval_120), frame_action_e::capture);
}

TEST(KmsFramePacing, NewFramebufferAfterFailedCaptureIsCapturedOnTheNextProbe) {
  auto pacing = paced_after_capture_at_t0();
  auto failed_at = t0 + frame_interval_120;
  ASSERT_EQ(platf::kms::pace_frame(pacing, 1, 0, false, failed_at, frame_interval_120).action, frame_action_e::capture);

  // The snapshot found no framebuffer: the loop keeps `pacing` and sleeps one probe interval.
  auto next_probe = platf::kms::frame_wait_deadline(pacing, failed_at);
  EXPECT_EQ(next_probe, failed_at + platf::kms::frame_probe_interval);
  EXPECT_EQ(platf::kms::pace_frame(pacing, 1, 2, false, next_probe, frame_interval_120).action, frame_action_e::capture);
}

TEST(KmsFramePacing, FasterDisplayNeverExceedsStreamRate) {
  // Physical monitor at 144 Hz streamed at 120 fps: a new framebuffer is always pending.
  auto actions = simulate(std::chrono::nanoseconds {1'000'000'000LL / 144}, 300us, 10s);

  // At most one capture more than the stream rate in any second, exactly the stream rate once the
  // catch-up allowance is spent, and never two captures closer than the minimum interval.
  EXPECT_LE(busiest_second(actions), 121U);
  std::map<long, std::size_t> captures_per_second;
  for (std::size_t i = 0; i < actions.size(); ++i) {
    EXPECT_EQ(actions[i].action, frame_action_e::capture);
    ++captures_per_second[static_cast<long>((actions[i].at - t0) / 1s)];
    if (i > 0) {
      EXPECT_GE(actions[i].at - actions[i - 1].at, frame_interval_120 - platf::kms::frame_capture_tolerance);
    }
  }
  for (long second = 1; second < 10; ++second) {
    EXPECT_EQ(captures_per_second[second], 120U) << "second " << second;
  }
}

TEST(KmsFramePacing, MatchingDisplayCapturesEveryFrameExactlyOnce) {
  // 2728x2048 virtual display measured at 119.9986 Hz against a 120 fps stream.
  const std::chrono::nanoseconds display_period {static_cast<std::int64_t>(1e9 / 119.9986)};
  // Frames the capture needs to catch up when the first flip lands just after the first capture.
  constexpr std::size_t catch_up_captures = 10;

  for (auto jitter : {0us, 400us}) {
    for (auto phase : {0us, 130us, 270us, 410us, 4170us, 8000us}) {
      SCOPED_TRACE(testing::Message() << "jitter " << jitter.count() << "us, phase " << phase.count() << "us");
      auto actions = simulate(display_period, phase, 5s, jitter);

      for (const auto &action : actions) {
        EXPECT_EQ(action.action, frame_action_e::capture);
      }
      expect_every_frame_once(actions, catch_up_captures, platf::kms::frame_probe_interval);
      EXPECT_LE(busiest_second(actions), 121U);
    }
  }
}

TEST(KmsFramePacing, SixtyHertzContentIsCapturedOnceWithRepeatsBetweenFrames) {
  // 60 fps content on a 120 Hz display: a new framebuffer every other flip.
  const std::chrono::nanoseconds content_period {static_cast<std::int64_t>(2e9 / 119.9986)};

  for (auto phase : {0us, 130us, 4170us, 12000us}) {
    SCOPED_TRACE(testing::Message() << "phase " << phase.count() << "us");
    auto actions = simulate(content_period, phase, 5s);

    expect_every_frame_once(actions, 10, platf::kms::frame_probe_interval);
    EXPECT_LE(busiest_second(actions), 121U);
    std::size_t repeats = 0;
    for (const auto &action : actions) {
      repeats += action.action == frame_action_e::repeat;
    }
    EXPECT_GE(repeats, 5U * 60 - 2);
  }
}

TEST(KmsFramePacing, RepeatsAndCapturesTogetherStayWithinTheStreamRate) {
  // Content slightly slower than the repeat delay: new frames keep landing just after a repeat.
  const std::chrono::nanoseconds content_period = frame_interval_120 * platf::kms::frame_repeat_delay_half_frames / 2 + 600us;

  auto actions = simulate(content_period, 300us, 10s);

  std::size_t repeats = 0;
  for (const auto &action : actions) {
    repeats += action.action == frame_action_e::repeat;
  }
  EXPECT_GE(repeats, 100U);
  EXPECT_LE(busiest_second(actions), 121U);
  expect_every_frame_once(actions, 2, frame_interval_120 / 2);
}

#endif
