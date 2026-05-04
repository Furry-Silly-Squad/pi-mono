#include "subagent.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

int fail(const char* msg) {
    std::cerr << "FAIL: " << msg << "\n";
    return 1;
}

int pass() {
    return 0;
}

}  // namespace

int main() {
    using namespace coding_agent;

    // --- DAG Validation Tests ---

    // Empty graph is valid
    {
        std::vector<SubTask> subtasks;
        auto error = validateSubtaskDag(subtasks);
        if (error.has_value()) {
            return fail("empty graph should be valid");
        }
    }

    // Single task without dependencies is valid
    {
        std::vector<SubTask> subtasks{
            {"1", "Task 1", {}, {}, {}, 1, ""},
        };
        auto error = validateSubtaskDag(subtasks);
        if (error.has_value()) {
            return fail("single task should be valid");
        }
    }

    // Linear dependencies are valid
    {
        std::vector<SubTask> subtasks{
            {"1", "Task 1", {}, {}, {}, 1, ""},
            {"2", "Task 2", {}, {}, {"1"}, 2, ""},
            {"3", "Task 3", {}, {}, {"2"}, 3, ""},
        };
        auto error = validateSubtaskDag(subtasks);
        if (error.has_value()) {
            return fail("linear dependencies should be valid");
        }
    }

    // Diamond dependencies are valid
    {
        std::vector<SubTask> subtasks{
            {"1", "Task 1", {}, {}, {}, 1, ""},
            {"2", "Task 2", {}, {}, {"1"}, 2, ""},
            {"3", "Task 3", {}, {}, {"1"}, 3, ""},
            {"4", "Task 4", {}, {}, {"2", "3"}, 4, ""},
        };
        auto error = validateSubtaskDag(subtasks);
        if (error.has_value()) {
            return fail("diamond dependencies should be valid");
        }
    }

    // Self-dependency is invalid
    {
        std::vector<SubTask> subtasks{
            {"1", "Task 1", {}, {}, {"1"}, 1, ""},
        };
        auto error = validateSubtaskDag(subtasks);
        if (!error.has_value() || error->find("depends on itself") == std::string::npos) {
            return fail("self-dependency should be invalid");
        }
    }

    // Missing dependency is invalid
    {
        std::vector<SubTask> subtasks{
            {"1", "Task 1", {}, {}, {}, 1, ""},
            {"2", "Task 2", {}, {}, {"99"}, 2, ""},
        };
        auto error = validateSubtaskDag(subtasks);
        if (!error.has_value() || error->find("unknown task") == std::string::npos) {
            return fail("missing dependency should be invalid");
        }
    }

    // Cycle of two is invalid
    {
        std::vector<SubTask> subtasks{
            {"1", "Task 1", {}, {}, {"2"}, 1, ""},
            {"2", "Task 2", {}, {}, {"1"}, 2, ""},
        };
        auto error = validateSubtaskDag(subtasks);
        if (!error.has_value() || error->find("cycle") == std::string::npos) {
            return fail("cycle of two should be invalid");
        }
    }

    // Cycle of three is invalid
    {
        std::vector<SubTask> subtasks{
            {"1", "Task 1", {}, {}, {"3"}, 1, ""},
            {"2", "Task 2", {}, {}, {"1"}, 2, ""},
            {"3", "Task 3", {}, {}, {"2"}, 3, ""},
        };
        auto error = validateSubtaskDag(subtasks);
        if (!error.has_value() || error->find("cycle") == std::string::npos) {
            return fail("cycle of three should be invalid");
        }
    }

    // --- Topological Sort Tests ---

    // Single task returns itself
    {
        std::vector<SubTask> subtasks{
            {"1", "Task 1", {}, {}, {}, 1, ""},
        };
        auto order = topologicalSortSubtasks(subtasks);
        if (!order.has_value() || order->size() != 1 || (*order)[0] != "1") {
            return fail("single task topological sort should return itself");
        }
    }

    // Linear order is preserved
    {
        std::vector<SubTask> subtasks{
            {"1", "Task 1", {}, {}, {}, 1, ""},
            {"2", "Task 2", {}, {}, {"1"}, 2, ""},
            {"3", "Task 3", {}, {}, {"2"}, 3, ""},
        };
        auto order = topologicalSortSubtasks(subtasks);
        if (!order.has_value() || order->size() != 3 ||
            (*order)[0] != "1" || (*order)[1] != "2" || (*order)[2] != "3") {
            return fail("linear topological sort order incorrect");
        }
    }

    // Diamond order respects dependencies
    {
        std::vector<SubTask> subtasks{
            {"1", "Task 1", {}, {}, {}, 1, ""},
            {"2", "Task 2", {}, {}, {"1"}, 2, ""},
            {"3", "Task 3", {}, {}, {"1"}, 3, ""},
            {"4", "Task 4", {}, {}, {"2", "3"}, 4, ""},
        };
        auto order = topologicalSortSubtasks(subtasks);
        if (!order.has_value() || order->size() != 4) {
            return fail("diamond topological sort should have 4 tasks");
        }
        auto it1 = std::find(order->begin(), order->end(), "1");
        auto it2 = std::find(order->begin(), order->end(), "2");
        auto it3 = std::find(order->begin(), order->end(), "3");
        auto it4 = std::find(order->begin(), order->end(), "4");
        if (it1 >= it2 || it1 >= it3 || it2 >= it4 || it3 >= it4) {
            return fail("diamond topological sort should respect dependencies");
        }
    }

    // Priority ordering: lower priority number = higher priority
    {
        std::vector<SubTask> subtasks{
            {"1", "Task 1", {}, {}, {}, 1, ""},
            {"2", "Task 2", {}, {}, {"1"}, 3, ""},
            {"3", "Task 3", {}, {}, {"1"}, 2, ""},
        };
        auto order = topologicalSortSubtasks(subtasks);
        if (!order.has_value() || order->size() != 3 || (*order)[0] != "1") {
            return fail("priority topological sort: first task should be 1");
        }
        auto it2 = std::find(order->begin(), order->end(), "2");
        auto it3 = std::find(order->begin(), order->end(), "3");
        if (it3 >= it2) {
            return fail("priority topological sort: task 3 should come before task 2");
        }
    }

    // Independent tasks sorted by priority
    {
        std::vector<SubTask> subtasks{
            {"3", "Task 3", {}, {}, {}, 3, ""},
            {"1", "Task 1", {}, {}, {}, 1, ""},
            {"2", "Task 2", {}, {}, {}, 2, ""},
        };
        auto order = topologicalSortSubtasks(subtasks);
        if (!order.has_value() || order->size() != 3 ||
            (*order)[0] != "1" || (*order)[1] != "2" || (*order)[2] != "3") {
            return fail("independent tasks should be sorted by priority");
        }
    }

    // Returns nullopt for cycles
    {
        std::vector<SubTask> subtasks{
            {"1", "Task 1", {}, {}, {"2"}, 1, ""},
            {"2", "Task 2", {}, {}, {"1"}, 2, ""},
        };
        auto order = topologicalSortSubtasks(subtasks);
        if (order.has_value()) {
            return fail("cycle should return nullopt from topological sort");
        }
    }

    // Returns nullopt for self-dependency
    {
        std::vector<SubTask> subtasks{
            {"1", "Task 1", {}, {}, {"1"}, 1, ""},
        };
        auto order = topologicalSortSubtasks(subtasks);
        if (order.has_value()) {
            return fail("self-dependency should return nullopt from topological sort");
        }
    }

    std::cout << "coding-agent-subagent-dag-test: ok\n";
    return pass();
}
