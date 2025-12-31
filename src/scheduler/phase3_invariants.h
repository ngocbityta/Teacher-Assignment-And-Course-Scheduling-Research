#pragma once

#include "phase3.h"
#include "phase1.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace phase3 {

// Cache structure for fast lookups (day/period indices and required_periods)
struct CachedIndices {
    std::unordered_map<std::string, int> day_to_idx;
    std::unordered_map<std::string, int> period_to_idx;
    std::unordered_map<std::string, int> course_section_to_periods; // "course_id|section_id" -> required_periods
    int num_days;
    int num_periods;
    int total_slots;
    
    CachedIndices(const ProblemData &data);
    
    int get_day_idx(const std::string &day) const;
    int get_period_idx(const std::string &period) const;
    int get_required_periods(const std::string &course_id, const std::string &section_id) const;
};

// Cache structure for fast lookups of classrooms and teachers
struct CachedLookups {
    std::unordered_map<std::string, int> classroom_capacity; // classroom_id -> capacity
    std::unordered_map<std::string, std::unordered_set<std::string>> teacher_eligible_courses; // teacher_id -> set of course_ids
    std::unordered_map<std::string, int> course_section_seats; // "course_id|section_id" -> required_seats
    
    CachedLookups(const ProblemData &data);
    
    bool classroom_exists(const std::string &classroom_id) const;
    int get_classroom_capacity(const std::string &classroom_id) const;
    bool is_teacher_eligible(const std::string &teacher_id, const std::string &course_id) const;
    int get_required_seats(const std::string &course_id, const std::string &section_id) const;
};

// Centralized hard constraint checking
// These are the HARD CORE constraints that must NEVER be violated:
// 1. Every assignment must have a non-empty classroom_id
// 2. No room-time conflict
// 3. No teacher-time conflict
// 4. No course/section-time conflict
// 5. Period continuity for multi-period lectures
// 6. Teacher eligibility for course
// 7. Teacher max_courses constraint (teacher cannot teach more than max_courses distinct courses)
// 8. Teacher workload constraint (teacher total periods <= max_load, if max_load is defined)
// 9. Course min/max teachers constraint (course must have between min_teachers and max_teachers distinct teachers)

// Check if a single assignment violates hard constraints
// Returns empty string if valid, error message if invalid
std::string check_assignment_hard_invariant(
    const OptimalSolution::Assignment &assignment,
    const ProblemData &data,
    int assignment_index = -1);

// Check if the entire solution satisfies all hard constraints
// Returns true if all constraints satisfied, false otherwise
bool check_hard_invariant(
    const OptimalSolution &solution,
    const ProblemData &data);

// Count hard constraint violations in a solution
// Returns number of violations (0 = all satisfied)
int count_hard_violations(
    const OptimalSolution &solution,
    const ProblemData &data);

// Check teacher eligibility (HARD CONSTRAINT)
bool is_teacher_eligible(
    const ProblemData &data,
    const std::string &teacher_id,
    const std::string &course_id);

// Report detailed constraint violations
// Prints detailed information about which constraints are violated
void report_constraint_violations(
    const OptimalSolution &solution,
    const ProblemData &data);

} // namespace phase3

