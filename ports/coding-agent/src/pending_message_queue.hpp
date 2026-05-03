#pragma once

#include <string>
#include <vector>

namespace coding_agent {

// ============================================================================
// Queue Modes
// ============================================================================

/// How queue messages are drained.
enum class QueueMode {
    OneAtATime,  ///< Return only the first item, leave rest
    All,         ///< Return all items, clear queue
};

// ============================================================================
// Streaming Behavior
// ============================================================================

/// How to handle messages received while the agent is streaming.
enum class StreamingBehavior {
    Steer,   ///< Queue as steering (delivered before next LLM call)
    FollowUp ///< Queue as follow-up (delivered only when agent would stop)
};

// ============================================================================
// PendingMessageQueue
// ============================================================================

/// Simple message queue with mode-aware drain strategy.
class PendingMessageQueue {
public:
    explicit PendingMessageQueue(QueueMode mode = QueueMode::OneAtATime)
        : mode_(mode) {}

    void enqueue(const std::string& text,
                 const std::vector<std::string>& images = {}) {
        items_.emplace_back(text, images);
    }

    bool has_items() const { return !items_.empty(); }

    void clear() { items_.clear(); }

    /// Drain messages respecting mode:
    ///   OneAtATime: returns only the first item, leaves rest
    ///   All: returns all items, clears queue
    std::vector<std::pair<std::string, std::vector<std::string>>> drain() {
        if (items_.empty()) return {};

        if (mode_ == QueueMode::All) {
            std::vector<std::pair<std::string, std::vector<std::string>>> result;
            result.swap(items_);
            return result;
        }

        // OneAtATime: return only the first item
        auto result = std::vector<std::pair<std::string, std::vector<std::string>>>{};
        result.push_back(std::move(items_.front()));
        items_.erase(items_.begin());
        return result;
    }

    QueueMode mode() const { return mode_; }
    void set_mode(QueueMode mode) { mode_ = mode; }

    size_t size() const { return items_.size(); }

    /// Return a copy of items without draining (for display purposes).
    const std::vector<std::pair<std::string, std::vector<std::string>>>& items() const {
        return items_;
    }

private:
    QueueMode mode_;
    std::vector<std::pair<std::string, std::vector<std::string>>> items_;
};

}  // namespace coding_agent
