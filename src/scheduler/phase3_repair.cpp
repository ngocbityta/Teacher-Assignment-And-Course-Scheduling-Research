#include "phase3_repair.h"
#include "phase3_index.h"
#include "phase3_move_evaluator.h"
#include "phase3_penalty_state.h"
#include "phase3_invariants.h"
#include "phase_common.h"
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cassert>
#include <iostream>
#include <vector>

namespace phase3 {

// Simplified repair that ONLY fixes:
// 1. Classroom-time conflicts (reassign rooms)
// 2. Max_teachers violations (remove excess teachers)
// Does NOT move assignments to different time slots to avoid creating teacher-time conflicts

OptimalSolution final_repair_solution(const OptimalSolution &solution, const ProblemData &data) {
    OptimalSolution repaired = solution;
    
    std::cout << "[REPAIR] Starting simplified repair phase..." << std::endl;
    
    // Step 1: Fix classroom-time conflicts by reassigning rooms
    std::cout << "[REPAIR] Step 1: Fixing classroom-time conflicts..." << std::endl;
    
    // Build room usage map: (room_id, day_idx, period_idx) -> list of assignment indices
    std::map<std::tuple<std::string, int, int>, std::vector<int>> room_usage;
    
    for (size_t i = 0; i < repaired.assignments.size(); ++i) {
        const auto &a = repaired.assignments[i];
        if (a.classroom_id.empty()) continue;
        
        int day_idx = find_day_index(data.classrooms.days, a.day);
        int period_idx = find_period_index(data.classrooms.periods, a.period);
        int required_periods = get_required_periods(data, a.course_id, a.section_id);
        
        if (day_idx >= 0 && period_idx >= 0) {
            for (int p = 0; p < required_periods; ++p) {
                int period_slot = period_idx + p;
                if (period_slot < (int)data.classrooms.periods.size()) {
                    auto key = std::make_tuple(a.classroom_id, day_idx, period_slot);
                    room_usage[key].push_back((int)i);
                }
            }
        }
    }
    
    // Iteratively fix classroom conflicts until none remain
    int classroom_conflicts_fixed = 0;
    const int MAX_ITERATIONS = 10;
    
    for (int iter = 0; iter < MAX_ITERATIONS; ++iter) {
        // Rebuild room usage map
        room_usage.clear();
        for (size_t i = 0; i < repaired.assignments.size(); ++i) {
            const auto &a = repaired.assignments[i];
            if (a.classroom_id.empty()) continue;
            
            int day_idx = find_day_index(data.classrooms.days, a.day);
            int period_idx = find_period_index(data.classrooms.periods, a.period);
            int required_periods = get_required_periods(data, a.course_id, a.section_id);
            
            if (day_idx >= 0 && period_idx >= 0) {
                for (int p = 0; p < required_periods; ++p) {
                    int period_slot = period_idx + p;
                    if (period_slot < (int)data.classrooms.periods.size()) {
                        auto key = std::make_tuple(a.classroom_id, day_idx, period_slot);
                        room_usage[key].push_back((int)i);
                    }
                }
            }
        }
        
        // Check if there are any conflicts
        bool has_conflicts = false;
        for (const auto &[key, assignment_indices] : room_usage) {
            if (assignment_indices.size() > 1) {
                has_conflicts = true;
                break;
            }
        }
        
        if (!has_conflicts) {
            std::cout << "[REPAIR] All classroom conflicts resolved after " << iter << " iterations" << std::endl;
            break;
        }
        
        // Try to resolve conflicts this iteration
        bool fixed_any = false;
        for (const auto &[key, assignment_indices] : room_usage) {
            if (assignment_indices.size() <= 1) continue; // No conflict
            
            const auto &[room_id, day_idx, period_slot] = key;
            
            // Keep first assignment in this room, reassign others
            for (size_t i = 1; i < assignment_indices.size(); ++i) {
                int assign_idx = assignment_indices[i];
                auto &a = repaired.assignments[assign_idx];
                
                int required_seats = get_required_seats(data, a.course_id, a.section_id);
                int required_periods = get_required_periods(data, a.course_id, a.section_id);
                int day_idx_a = find_day_index(data.classrooms.days, a.day);
                int period_idx_a = find_period_index(data.classrooms.periods, a.period);
                
                // Try to find an available room with sufficient capacity
                bool reassigned = false;
                for (const auto &room : data.classrooms.classrooms) {
                    if (room.id == a.classroom_id) continue; // Current room
                    if (room.capacity < required_seats) continue; // Too small
                    
                    // Check if room is available for all required periods
                    bool available = true;
                    for (int p = 0; p < required_periods && available; ++p) {
                        int period_check = period_idx_a + p;
                        if (period_check >= (int)data.classrooms.periods.size()) {
                            available = false;
                            break;
                        }
                        
                        auto check_key = std::make_tuple(room.id, day_idx_a, period_check);
                        auto it = room_usage.find(check_key);
                        if (it != room_usage.end() && !it->second.empty()) {
                            available = false; // Room already occupied
                        }
                    }
                    
                    if (available) {
                        // Reassign to this room
                        a.classroom_id = room.id;
                        classroom_conflicts_fixed++;
                        fixed_any = true;
                        reassigned = true;
                        break; // Move to next conflict
                    }
                }
                
                if (reassigned) {
                    break; // Only fix one assignment per conflicted slot per iteration
                }
            }
        }
        
        if (!fixed_any) {
            std::cout << "[REPAIR] No more conflicts can be resolved (iteration " << iter << ")" << std::endl;
            break;
        }
    }
    
    std::cout << "[REPAIR] Fixed " << classroom_conflicts_fixed << " classroom conflicts" << std::endl;
    
    // Step 2: Fix max_teachers violations by removing excess teachers
    std::cout << "[REPAIR] Step 2: Fixing max_teachers violations..." << std::endl;
    
    // Count teachers per course
    std::map<std::string, std::map<std::string, int>> course_teacher_count; // course_id -> teacher_id -> count
    std::map<std::string, int> course_teacher_total; // course_id -> total teachers
    
    for (const auto &a : repaired.assignments) {
        course_teacher_count[a.course_id][a.teacher_id]++;
    }
    
    for (const auto &[course_id, teachers] : course_teacher_count) {
        course_teacher_total[course_id] = (int)teachers.size();
    }
    
    // Find courses exceeding max_teachers
    std::vector<int> to_remove; // Indices of assignments to remove
    int max_teachers_fixed = 0;
    
    for (const auto &course : data.courses) {
        if (course.max_teachers <= 0) continue;
        
        int current_teachers = course_teacher_total[course.id];
        if (current_teachers <= course.max_teachers) continue;
        
        int excess = current_teachers - course.max_teachers;
        std::cout << "[REPAIR] Course " << course.id << " has " << current_teachers 
                  << " teachers, max is " << course.max_teachers 
                  << " (excess: " << excess << ")" << std::endl;
        
        // Collect all teachers for this course with their assignment counts
        std::vector<std::pair<int, std::string>> teacher_counts; // (count, teacher_id)
        for (const auto &[teacher_id, count] : course_teacher_count[course.id]) {
            teacher_counts.push_back({count, teacher_id});
        }
        
        // Sort by count (ascending) - remove teachers with fewest assignments first
        std::sort(teacher_counts.begin(), teacher_counts.end());
        
        // Remove excess teachers
        for (int t = 0; t < excess && t < (int)teacher_counts.size(); ++t) {
            const std::string &teacher_to_remove = teacher_counts[t].second;
            
            // Mark all assignments of this teacher for this course for removal
            for (size_t i = 0; i < repaired.assignments.size(); ++i) {
                const auto &a = repaired.assignments[i];
                if (a.course_id == course.id && a.teacher_id == teacher_to_remove) {
                    to_remove.push_back((int)i);
                    max_teachers_fixed++;
                }
            }
        }
    }
    
    // Remove marked assignments (in reverse order to preserve indices)
    std::sort(to_remove.rbegin(), to_remove.rend());
    for (int idx : to_remove) {
        repaired.assignments.erase(repaired.assignments.begin() + idx);
    }
    
    std::cout << "[REPAIR] Removed " << to_remove.size() << " assignments to fix max_teachers violations" << std::endl;
    std::cout << "[REPAIR] Total fixes: " << classroom_conflicts_fixed << " classroom, " 
              << to_remove.size() << " max_teachers" << std::endl;
    
    return repaired;
}

bool check_all_constraints(const OptimalSolution &sol, const ProblemData &data) {
    // Use the centralized function from phase3_invariants
    return check_hard_invariant(sol, data);
}

} // namespace phase3
