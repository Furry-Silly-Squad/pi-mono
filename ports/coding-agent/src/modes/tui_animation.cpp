#include "modes/tui_animation.hpp"

#include <iostream>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace coding_agent {
namespace {

const std::vector<std::string> spinner_frames = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};

const char* state_label(AnimationState state) {
  switch (state) {
    case AnimationState::Thinking:
      return "thinking";
    case AnimationState::Generating:
      return "generating";
    case AnimationState::Running:
      return "running";
    case AnimationState::Idle:
    default:
      return "";
  }
}

}  // namespace

const std::vector<std::string> TuiAnimation::spinner_frames_ = spinner_frames;

TuiAnimation::TuiAnimation() {}

TuiAnimation::~TuiAnimation() {
  stop();
}

void TuiAnimation::start(AnimationState state, const std::string& label) {
  if (running_.load()) {
    return;  // Already running
  }
  current_state_.store(state);
  current_label_ = label;
  frame_index_ = 0;
  running_.store(true);
  thread_ = std::thread(&TuiAnimation::animation_thread, this);
}

void TuiAnimation::stop() {
  if (!running_.load()) {
    return;
  }
  running_.store(false);
  if (thread_.joinable()) {
    thread_.join();
  }
  // Clear the animation line
  std::cout << CURSOR_SHOW << CLEAR_LINE;
  std::cout.flush();
}

void TuiAnimation::update(AnimationState state, const std::string& label) {
  current_state_.store(state);
  current_label_ = label;
}

void TuiAnimation::animation_thread() {
  // Hide cursor
  std::cout << CURSOR_HIDE;
  std::cout.flush();

  const auto interval = std::chrono::milliseconds(100);  // 10Hz
  while (running_.load()) {
    // Build the display string
    std::ostringstream oss;
    oss << CLEAR_LINE << spinner_frames_[frame_index_ % spinner_frames_.size()]
        << " " << state_label(current_state_.load());
    if (!current_label_.empty()) {
      oss << ": " << current_label_;
    }

    // Write to stdout (thread-safe for short writes)
    const std::string display = oss.str();
    write(STDOUT_FILENO, display.c_str(), display.size());

    frame_index_++;
    std::this_thread::sleep_for(interval);
  }
}

}  // namespace coding_agent
