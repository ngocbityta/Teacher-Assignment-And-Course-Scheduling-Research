#pragma once

#include "phase3.h"
#include "phase3_move.h"
#include <unordered_map>
#include <unordered_set>
#include <string>

namespace phase3 {

constexpr int MAX_PERIOD_INDEX = 128;

// Soft constraint accounting only
// Tracks workload balance, compactness, and room conflict penalties
struct PenaltyState {
    std::unordered_map<std::string, int> workload;
    std::unordered_map<std::string, std::unordered_map<std::string, std::unordered_set<int>>> day_slots;
    // Room conflict tracking: (room_id, day_idx) -> set of busy periods
    std::unordered_map<std::string, std::unordered_map<int, std::unordered_set<int>>> room_slots;
    // Room capacity violations: count of assignments where required_seats > room_capacity
    int room_capacity_violations = 0;
    double workload_var = 0.0;
    double sum_workload = 0.0;
    double sum_workload_squared = 0.0;
    double compactness = 0.0;
    double room_conflict_penalty = 0.0; // Count of room-time conflicts
    
    double get_workload_penalty() const;
    double get_room_penalty() const; // Returns room_conflict_penalty + capacity_violations
    void update_workload_var_from_sums();
    static double compute_compactness_for_set(const std::unordered_set<int> &slots);
    double compute_compactness_for_teacher_day(const std::string &teacher_id, const std::string &day) const;
    void update_workload(const AssignmentChange &chg, const ProblemData &data);
    void update_compactness(const AssignmentChange &chg, const ProblemData &data);
    void update_room_conflicts(const AssignmentChange &chg, const ProblemData &data);
    // Update room penalty for a single assignment (for ROOM_SWAP/ROOM_SHIFT delta updates)
    void update_room_penalty_for_assignment(int assignment_idx, 
                                           const OptimalSolution::Assignment &assignment,
                                           const ProblemData &data);
    void apply_change(const AssignmentChange &chg, const ProblemData &data);
    void revert_change(const AssignmentChange &chg, const ProblemData &data);
};

// Initialize penalty state from solution
PenaltyState init_penalty_state(const OptimalSolution &sol, const ProblemData &data);

} // namespace phase3

