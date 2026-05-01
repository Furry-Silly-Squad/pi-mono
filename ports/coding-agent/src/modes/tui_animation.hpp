#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace coding_agent {

enum class AnimationState {
  Idle,      // No animation running
  Thinking,  // Waiting for LLM to start generating
  Generating, // LLM is streaming content
  Running,   // Running a tool
};

class TuiAnimation {
 public:
  TuiAnimation();
  ~TuiAnimation();

  // Start the animation with a given state and label
  void start(AnimationState state, const std::string& label);

  // Stop the animation and clear the display line
  void stop();

  // Update the animation state and label (thread-safe)
  void update(AnimationState state, const std::string& label);

 private:
  void animation_thread();

  std::atomic<bool> running_{false};
  std::thread thread_;

  std::atomic<AnimationState> current_state_{AnimationState::Idle};
  std::string current_label_;

  // Unicode spinner frames
  static const std::vector<std::string> spinner_frames_;
  size_t frame_index_ = 0;

  // ANSI escape codes
  static constexpr const char* CURSOR_HIDE = "\033[?25l";
  static constexpr const char* CURSOR_SHOW = "\033[?25h";
  static constexpr const char* CLEAR_LINE = "\033[2K\r";
};

}  // namespace coding_agent
