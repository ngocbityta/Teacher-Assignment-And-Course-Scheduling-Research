#pragma once

#include "phase3.h"
#include "phase3_move.h"
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>
#include <tuple>

namespace phase3 {

// Solution index for efficient conflict checking
struct SolIndex {
    std::unordered_map<std::string, std::unordered_map<int, int>> teacher_busy_idx;
    std::unordered_map<std::string, std::unordered_map<int, int>> course_slot_section_idx;
    std::unordered_map<std::string, std::unordered_set<std::string>> teacher_courses; // teacher_id -> set of course_ids
    std::unordered_map<std::string, std::unordered_set<std::string>> course_teachers;
    std::unordered_map<std::string, std::unordered_set<std::string>> course_teacher_sections;
    std::unordered_map<std::string, std::unordered_map<int, int>> classroom_busy_idx;
    int num_periods = 0;
};

// Index delta for incremental updates
enum class DeltaMode {
    ADD,
    REMOVE,
    DIFF
};

struct IndexDelta {
    std::vector<std::pair<std::string, int>> teacher_add_idx;
    std::vector<std::pair<std::string, int>> teacher_remove_idx;
    std::vector<std::pair<std::string, int>> classroom_add_idx;
    std::vector<std::pair<std::string, int>> classroom_remove_idx;
    std::vector<std::tuple<std::string, int, std::string>> course_slot_section_add_idx;
    std::vector<std::tuple<std::string, int, std::string>> course_slot_section_remove_idx;
    std::vector<std::pair<std::string, std::string>> course_teacher_add;
    std::vector<std::pair<std::string, std::string>> course_teacher_remove;
    std::vector<std::tuple<std::string, std::string, std::string>> course_teacher_section_add;
    std::vector<std::tuple<std::string, std::string, std::string>> course_teacher_section_remove;
    
    IndexDelta& operator+=(const IndexDelta& other);
};

// Build index from solution
SolIndex build_index(const OptimalSolution &sol, const ProblemData &data);

// Apply index delta incrementally
void apply_index_delta(SolIndex &idx, const IndexDelta &d);

// Build delta for an assignment change
IndexDelta build_delta(
    const OptimalSolution::Assignment &old_a,
    const OptimalSolution::Assignment &new_a,
    DeltaMode mode,
    const ProblemData &data);

} // namespace phase3

