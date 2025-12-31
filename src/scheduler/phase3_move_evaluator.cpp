#include "phase3_move_evaluator.h"
#include "phase3_index.h"
#include "phase3_invariants.h"
#include "phase3_penalty_state.h"
#include "phase_common.h"
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cmath>

namespace phase3 {

struct Slot {
    int day_idx;
    int period_idx;
};

static inline int safe_slot_index(int day_idx, int period_idx, int num_periods, const char* context = "") {
    if (day_idx < 0 || period_idx < 0 || num_periods <= 0) return -1;
    if (day_idx >= 1000 || period_idx >= num_periods) return -1;
    int slot_idx = day_idx * num_periods + period_idx;
    if (slot_idx < 0) return -1;
    return slot_idx;
}

static inline int slot_index(int day_idx, int period_idx, int num_periods) {
    return safe_slot_index(day_idx, period_idx, num_periods, "move_evaluator:slot_index");
}

template<typename F>
static inline void for_each_slot(const OptimalSolution::Assignment &a, const CachedIndices &cache, F fn) {
    int day_idx = cache.get_day_idx(a.day);
    int period_idx = cache.get_period_idx(a.period);
    int required_periods = cache.get_required_periods(a.course_id, a.section_id);
    
    if (day_idx < 0 || period_idx < 0) return;
    if (period_idx + required_periods > cache.num_periods) return;
    
    for (int i = 0; i < required_periods; ++i) {
        fn(Slot{day_idx, period_idx + i});
    }
}

static inline std::string get_assignment_id(const OptimalSolution::Assignment &a) {
    return a.course_id + "|" + a.section_id;
}

std::string MoveEvaluator::is_feasible(const OptimalSolution::Assignment &a,
                                       const SolIndex &idx, 
                                       const ProblemData &data,
                                       const CachedIndices &cache,
                                       const CachedLookups &lookups) {
    int day_idx = cache.get_day_idx(a.day);
    int period_idx = cache.get_period_idx(a.period);
    int required_periods = cache.get_required_periods(a.course_id, a.section_id);
    
    if (day_idx < 0 || period_idx < 0 || period_idx + required_periods > cache.num_periods) {
        return "";
    }
    
    auto it_teacher = idx.teacher_busy_idx.find(a.teacher_id);
    if (it_teacher != idx.teacher_busy_idx.end()) {
        bool has_clash = false;
        for_each_slot(a, cache, [&](const Slot &slot) {
            int slot_idx = safe_slot_index(slot.day_idx, slot.period_idx, idx.num_periods, "is_feasible:teacher");
            if (slot_idx >= 0 && it_teacher->second.count(slot_idx)) has_clash = true;
        });
        if (has_clash) return "";
    }
    
    auto it_course = idx.course_slot_section_idx.find(a.course_id);
    if (it_course != idx.course_slot_section_idx.end()) {
        bool has_clash = false;
        for_each_slot(a, cache, [&](const Slot &slot) {
            int slot_idx = safe_slot_index(slot.day_idx, slot.period_idx, idx.num_periods, "is_feasible:course");
            if (slot_idx >= 0) {
                auto it_slot = it_course->second.find(slot_idx);
                if (it_slot != it_course->second.end()) {
                    has_clash = true;
                }
            }
        });
        if (has_clash) return "";
    }
    
    if (data.classrooms.classrooms.empty()) return "";

    // OPTIMIZATION: Check current room first
    if (!a.classroom_id.empty()) {
        auto it_room = idx.classroom_busy_idx.find(a.classroom_id);
        bool available = true;
        for_each_slot(a, cache, [&](const Slot &slot) {
            int slot_idx = safe_slot_index(slot.day_idx, slot.period_idx, idx.num_periods, "is_feasible:room");
            // Check if room is busy by ANOTHER assignment
            if (slot_idx >= 0 && it_room != idx.classroom_busy_idx.end() && it_room->second.count(slot_idx)) {
                // Determine if the clash is with SELF (which is fine, we are moving) 
                // BUT idx.classroom_busy_idx stores indices of assignments.
                // Wait, is_feasible takes 'a' which is a candidate assignment. 
                // Ideally we should know which assignment index we are modifying to ignore self-clashes.
                // But idx stores simple busy flags or assignment indices?
                // Checking idx definition... Phase3Index uses unordered_map<string, unordered_set<int>> for room busy.
                // It tracks busy SLOTS, not who occupies them. (Wait, let's verify Phase3Index)
                
                // Assuming we cannot distinguish self-clash here easily without index update logic.
                // However, moves usually remove old assignment from index before checking?
                // No, MoveEvaluator checks feasibility against CURRENT index.
                // If we are MOVING an existing assignment, its old slots are in the index.
                // If we keep the same room and same time, it clashes with self?
                // But we are changing time or teacher.
                // If we change time, the new time slots in the same room might be busy.
                
                // For now, assume if slot is busy, it's busy.
                available = false;
            }
        });
        
        // Use capacity check from data (room id must exist)
        // We need to find the room object to check capacity
        bool capacity_ok = false;
        for (const auto &room : data.classrooms.classrooms) {
            if (room.id == a.classroom_id) {
                if (room.capacity >= get_required_seats(data, a.course_id, a.section_id)) {
                    capacity_ok = true;
                }
                break;
            }
        }
        
        if (available && capacity_ok) return a.classroom_id;
    }
    
    // Fallback: Find any available room
    for (const auto &room : data.classrooms.classrooms) {
        // Skip current room as we already checked it (and it failed)
        if (room.id == a.classroom_id) continue;
        
        if (room.capacity < get_required_seats(data, a.course_id, a.section_id)) continue;
        
        auto it_room = idx.classroom_busy_idx.find(room.id);
        bool available = true;
        for_each_slot(a, cache, [&](const Slot &slot) {
            int slot_idx = safe_slot_index(slot.day_idx, slot.period_idx, idx.num_periods, "is_feasible:room");
            if (slot_idx >= 0 && it_room != idx.classroom_busy_idx.end() && it_room->second.count(slot_idx)) {
                available = false;
            }
        });
        if (available) return room.id;
    }
    return "";
}

std::string MoveEvaluator::is_feasible_with_removal(
    const OptimalSolution::Assignment &old_a,
    const OptimalSolution::Assignment &new_a,
    const SolIndex &idx,
    const ProblemData &data,
    const CachedIndices &cache,
    const CachedLookups &lookups) {
    int day_idx = cache.get_day_idx(new_a.day);
    int period_idx = cache.get_period_idx(new_a.period);
    int required_periods = cache.get_required_periods(new_a.course_id, new_a.section_id);
    
    if (day_idx < 0 || period_idx < 0 || period_idx + required_periods > cache.num_periods) {
        return "";
    }
    
    // HARD CONSTRAINT: Check teacher eligibility using CachedLookups
    if (!lookups.is_teacher_eligible(new_a.teacher_id, new_a.course_id)) {
        return "";
    }
    
    bool same_teacher = (old_a.teacher_id == new_a.teacher_id);
    bool same_course = (old_a.course_id == new_a.course_id);
    std::unordered_set<int> old_slots;
    for_each_slot(old_a, cache, [&](const Slot &slot) {
        int slot_idx = safe_slot_index(slot.day_idx, slot.period_idx, idx.num_periods, "is_feasible_with_removal:old");
        if (slot_idx >= 0) old_slots.insert(slot_idx);
    });
    
    auto it_teacher = idx.teacher_busy_idx.find(new_a.teacher_id);
    if (it_teacher != idx.teacher_busy_idx.end()) {
        bool has_clash = false;
        for_each_slot(new_a, cache, [&](const Slot &slot) {
            int slot_idx = safe_slot_index(slot.day_idx, slot.period_idx, idx.num_periods, "is_feasible_with_removal:teacher");
            if (slot_idx >= 0) {
                bool is_old_slot = old_slots.count(slot_idx) > 0;
                if (it_teacher->second.count(slot_idx) && (!same_teacher || !is_old_slot)) {
                    has_clash = true;
                }
            }
        });
        if (has_clash) return "";
    }
    
    auto it_course = idx.course_slot_section_idx.find(new_a.course_id);
    if (it_course != idx.course_slot_section_idx.end()) {
        bool has_clash = false;
        for_each_slot(new_a, cache, [&](const Slot &slot) {
            int slot_idx = safe_slot_index(slot.day_idx, slot.period_idx, idx.num_periods, "is_feasible_with_removal:course");
            if (slot_idx >= 0) {
                bool is_old_slot = old_slots.count(slot_idx) > 0;
                auto it_slot = it_course->second.find(slot_idx);
                // Check if slot is occupied by someone ELSE
                // If same_course and is_old_slot, we account for 1 count. Conflict if count > 1.
                // Otherwise, conflict if count > 0.
                if (it_slot != it_course->second.end()) {
                    int count = it_slot->second;
                    int ignored_count = (same_course && is_old_slot) ? 1 : 0;
                    if (count > ignored_count) {
                        has_clash = true;
                    }
                }
            }
        });
        if (has_clash) return "";
    }
    
    if (data.classrooms.classrooms.empty()) return "";
    
    bool old_has_classroom = !old_a.classroom_id.empty();
    for (const auto &room : data.classrooms.classrooms) {
        auto it_room = idx.classroom_busy_idx.find(room.id);
        bool available = true;
        bool same_classroom = (old_has_classroom && old_a.classroom_id == room.id);
        for_each_slot(new_a, cache, [&](const Slot &slot) {
            int slot_idx = safe_slot_index(slot.day_idx, slot.period_idx, idx.num_periods, "is_feasible_with_removal:room");
            if (slot_idx >= 0) {
                bool is_old_slot = old_slots.count(slot_idx) > 0;
                if (it_room != idx.classroom_busy_idx.end() && it_room->second.count(slot_idx) && (!same_classroom || !is_old_slot)) {
                    available = false;
                }
            }
        });
        if (available) return room.id;
    }
    return "";
}

TryAssignmentResult MoveEvaluator::try_assignment_change(
    const OptimalSolution::Assignment &old_assignment,
    OptimalSolution::Assignment new_assignment,
    const SolIndex &solution_index,
    const ProblemData &data,
    const CachedIndices &cache,
    const CachedLookups &lookups) {
    TryAssignmentResult result{false};
    
    result.classroom_id = is_feasible_with_removal(old_assignment, new_assignment, solution_index, data, cache, lookups);
    
    // HARD CONSTRAINT: Must have a valid classroom, no fallback allowed
    if (result.classroom_id.empty()) {
        return result; // ok = false, classroom_id = ""
    }
    
    new_assignment.classroom_id = result.classroom_id;
    
    result.delta = build_delta(old_assignment, new_assignment, DeltaMode::DIFF, data);
    
    result.ok = true;
    return result;
}

std::vector<AssignmentChange> MoveEvaluator::expand_move(
    const Move &move,
    const OptimalSolution &current_solution,
    const OptimalSolution &candidate_solution) {
    std::vector<int> affected_assignment_indices = move.indices;
    std::vector<AssignmentChange> assignment_changes;
    
    for (int assignment_idx : affected_assignment_indices) {
        if (assignment_idx < 0 || 
            assignment_idx >= (int)current_solution.assignments.size() || 
            assignment_idx >= (int)candidate_solution.assignments.size()) {
            continue;
        }
        
        const auto &old_assignment = current_solution.assignments[assignment_idx];
        const auto &new_assignment = candidate_solution.assignments[assignment_idx];
        
        bool assignment_changed = (old_assignment.teacher_id != new_assignment.teacher_id ||
                                  old_assignment.day != new_assignment.day ||
                                  old_assignment.period != new_assignment.period ||
                                  old_assignment.classroom_id != new_assignment.classroom_id);
        
        if (assignment_changed) {
            assignment_changes.push_back({assignment_idx, old_assignment, new_assignment});
        }
    }
    
    return assignment_changes;
}

MoveEvaluator::MoveEvaluator(const OptimalSolution &sol, const SolIndex &idx, const ProblemData &d,
                             const CachedIndices &c, const CachedLookups &l) 
    : current(sol), current_idx(idx), data(d), cache(c), lookups(l) {}

MoveContext MoveEvaluator::evaluate(const MoveSpec &spec) {
    MoveContext ctx{false};
    ctx.candidate = current;
    ctx.move.type = spec.type;
    ctx.move.indices = spec.assignment_indices;
    
    if (spec.type == Move::SINGLE_CHANGE || spec.type == Move::BLOCK_RELOCATE) {
        if (spec.assignment_indices.empty()) return ctx;
        int assignment_idx = spec.assignment_indices[0];
        if (assignment_idx < 0 || assignment_idx >= (int)current.assignments.size()) return ctx;
        
        const auto &old_assignment = current.assignments[assignment_idx];
        OptimalSolution::Assignment new_assignment = old_assignment;
        
        if (!spec.new_teacher_id.empty() && spec.new_teacher_id != old_assignment.teacher_id) {
            if (!lookups.is_teacher_eligible(spec.new_teacher_id, old_assignment.course_id)) {
                return ctx;
            }
            new_assignment.teacher_id = spec.new_teacher_id;
        } else if (!spec.new_day.empty() && !spec.new_period.empty()) {
            new_assignment.day = spec.new_day;
            new_assignment.period = spec.new_period;
        } else {
            return ctx;
        }
        
        if (old_assignment.classroom_id.empty()) {
            return ctx;
        }
        
        TryAssignmentResult assignment_result = try_assignment_change(
            old_assignment, new_assignment, current_idx, data, cache, lookups);
        
        // BUG FIX: Allow classroom changes - only check if move is feasible
        // Requiring same classroom is overly restrictive in dense problems
        if (!assignment_result.ok) {
            return ctx;
        }
        
        ctx.idx_after = current_idx;
        apply_index_delta(ctx.idx_after, assignment_result.delta);
        
        // Use the classroom found by try_assignment_change (may be different from old)
        new_assignment.classroom_id = assignment_result.classroom_id;
        ctx.candidate.assignments[assignment_idx] = new_assignment;
        
        ctx.ok = true;
        
    } else if (spec.type == Move::CHAIN_MOVE) {
        if (spec.chain.empty()) return ctx;
        
        for (const auto &chain_block : spec.chain) {
            if (chain_block.assignment_idx < 0 || chain_block.assignment_idx >= (int)current.assignments.size()) {
                return ctx;
            }
        }
        
        SolIndex trial_solution_index = current_idx;
        std::vector<OptimalSolution::Assignment> updated_assignments;
        IndexDelta combined_index_delta;
        
        for (const auto &chain_block : spec.chain) {
            const auto &old_assignment = current.assignments[chain_block.assignment_idx];
            OptimalSolution::Assignment new_assignment = old_assignment;
            
            new_assignment.period = chain_block.period;
            
            if (old_assignment.classroom_id.empty()) {
                return ctx;
            }
            
            TryAssignmentResult assignment_result = try_assignment_change(
                old_assignment, new_assignment, trial_solution_index, data, cache, lookups);
            
            // BUG FIX: Allow classroom changes in chain moves
            if (!assignment_result.ok) {
                return ctx;
            }
            
            // Use the classroom found by try_assignment_change
            new_assignment.classroom_id = assignment_result.classroom_id;
            updated_assignments.push_back(new_assignment);
            
            combined_index_delta += assignment_result.delta;
            
            apply_index_delta(trial_solution_index, assignment_result.delta);
        }
        
        ctx.candidate = current;
        for (size_t block_idx = 0; block_idx < spec.chain.size(); ++block_idx) {
            int assignment_idx = spec.chain[block_idx].assignment_idx;
            ctx.candidate.assignments[assignment_idx] = updated_assignments[block_idx];
        }
        
        ctx.move.chain = spec.chain;
        ctx.idx_after = current_idx;
        apply_index_delta(ctx.idx_after, combined_index_delta);
        ctx.ok = true;
        
    } else if (spec.type == Move::MULTI_SWAP) {
        if (spec.assignment_indices.empty() || spec.chain.empty() || 
            spec.assignment_indices.size() != spec.chain.size() ||
            spec.assignment_indices.size() != spec.new_days.size()) {
            return ctx;
        }
        
        // Verify all assignment indices are valid
        for (int idx : spec.assignment_indices) {
            if (idx < 0 || idx >= (int)current.assignments.size()) {
                return ctx;
            }
        }
        
        SolIndex trial_solution_index = current_idx;
        std::vector<OptimalSolution::Assignment> updated_assignments;
        IndexDelta combined_index_delta;
        
        // Apply swaps incrementally with hard constraint checking
        for (size_t i = 0; i < spec.assignment_indices.size(); ++i) {
            int assignment_idx = spec.assignment_indices[i];
            const auto &old_assignment = current.assignments[assignment_idx];
            OptimalSolution::Assignment new_assignment = old_assignment;
            
            new_assignment.day = spec.new_days[i];
            new_assignment.period = spec.chain[i].period;
            
            if (old_assignment.classroom_id.empty()) {
                return ctx;
            }
            
            TryAssignmentResult assignment_result = try_assignment_change(
                old_assignment, new_assignment, trial_solution_index, data, cache, lookups);
            
            // BUG FIX: Allow classroom changes in multi-swap
            // Incremental hard constraint check: if any swap fails, abort
            if (!assignment_result.ok) {
                return ctx;
            }
            
            // Use the classroom found by try_assignment_change
            new_assignment.classroom_id = assignment_result.classroom_id;
            updated_assignments.push_back(new_assignment);
            
            combined_index_delta += assignment_result.delta;
            apply_index_delta(trial_solution_index, assignment_result.delta);
        }
        
        ctx.candidate = current;
        for (size_t i = 0; i < spec.assignment_indices.size(); ++i) {
            int assignment_idx = spec.assignment_indices[i];
            ctx.candidate.assignments[assignment_idx] = updated_assignments[i];
        }
        
        ctx.move.chain = spec.chain;
        ctx.idx_after = current_idx;
        apply_index_delta(ctx.idx_after, combined_index_delta);
        ctx.ok = true;
        
    } else if (spec.type == Move::ROOM_SWAP) {
        // Swap rooms between two assignments
        if (spec.assignment_indices.size() != 2 || spec.new_room_ids.size() != 2) {
            return ctx;
        }
        
        int idx1 = spec.assignment_indices[0];
        int idx2 = spec.assignment_indices[1];
        
        if (idx1 < 0 || idx1 >= (int)current.assignments.size() ||
            idx2 < 0 || idx2 >= (int)current.assignments.size()) {
            return ctx;
        }
        
        const auto &old_a1 = current.assignments[idx1];
        const auto &old_a2 = current.assignments[idx2];
        
        if (old_a1.classroom_id.empty() || old_a2.classroom_id.empty()) {
            return ctx;
        }
        
        // Create new assignments with swapped rooms
        OptimalSolution::Assignment new_a1 = old_a1;
        OptimalSolution::Assignment new_a2 = old_a2;
        new_a1.classroom_id = spec.new_room_ids[0];
        new_a2.classroom_id = spec.new_room_ids[1];
        
        // Check if both assignments are feasible with new rooms
        SolIndex trial_idx = current_idx;
        
        // Check assignment 1 with new room
        TryAssignmentResult result1 = try_assignment_change(old_a1, new_a1, trial_idx, data, cache, lookups);
        if (!result1.ok || result1.classroom_id != new_a1.classroom_id) {
            return ctx;
        }
        
        // Update trial index
        apply_index_delta(trial_idx, result1.delta);
        
        // Check assignment 2 with new room
        TryAssignmentResult result2 = try_assignment_change(old_a2, new_a2, trial_idx, data, cache, lookups);
        if (!result2.ok || result2.classroom_id != new_a2.classroom_id) {
            return ctx;
        }
        
        // Both swaps are feasible
        ctx.candidate = current;
        ctx.candidate.assignments[idx1] = new_a1;
        ctx.candidate.assignments[idx2] = new_a2;
        
        ctx.idx_after = current_idx;
        IndexDelta combined_delta = result1.delta;
        combined_delta += result2.delta;
        apply_index_delta(ctx.idx_after, combined_delta);
        ctx.ok = true;
        
    } else if (spec.type == Move::ROOM_SHIFT) {
        // Change room for one assignment
        if (spec.assignment_indices.size() != 1 || spec.new_room_ids.size() != 1) {
            return ctx;
        }
        
        int assignment_idx = spec.assignment_indices[0];
        if (assignment_idx < 0 || assignment_idx >= (int)current.assignments.size()) {
            return ctx;
        }
        
        const auto &old_assignment = current.assignments[assignment_idx];
        if (old_assignment.classroom_id.empty()) {
            return ctx;
        }
        
        OptimalSolution::Assignment new_assignment = old_assignment;
        new_assignment.classroom_id = spec.new_room_ids[0];
        
        // Check if assignment is feasible with new room
        TryAssignmentResult result = try_assignment_change(
            old_assignment, new_assignment, current_idx, data, cache, lookups);
        
        if (!result.ok || result.classroom_id != new_assignment.classroom_id) {
            return ctx;
        }
        
        ctx.candidate = current;
        ctx.candidate.assignments[assignment_idx] = new_assignment;
        
        ctx.idx_after = current_idx;
        apply_index_delta(ctx.idx_after, result.delta);
        ctx.ok = true;
    }
    
    if (ctx.ok) ctx.changes = expand_move(ctx.move, current, ctx.candidate);
    return ctx;
}

int calculate_delta_hard(const MoveContext &ctx, const ProblemData &data,
                         const CachedIndices &cache, const CachedLookups &lookups,
                         const SolIndex &current_idx) {
    int delta = 0;
    
    // 1. Time Conflicts (Incremental)
    for (const auto &chg : ctx.changes) {
        // Decrease violations for old assignment (using current_idx)
        for_each_slot(chg.old_a, cache, [&](const Slot &slot) {
            int slot_idx = safe_slot_index(slot.day_idx, slot.period_idx, cache.num_periods, "calc_delta:old");
            if (slot_idx < 0) return;
            
            // Teacher
            {
                auto it = current_idx.teacher_busy_idx.find(chg.old_a.teacher_id);
                if (it != current_idx.teacher_busy_idx.end()) {
                    auto it_slot = it->second.find(slot_idx);
                    if (it_slot != it->second.end() && it_slot->second >= 2) delta--;
                }
            }
            // Classroom
            if (!chg.old_a.classroom_id.empty()) {
                auto it = current_idx.classroom_busy_idx.find(chg.old_a.classroom_id);
                if (it != current_idx.classroom_busy_idx.end()) {
                    auto it_slot = it->second.find(slot_idx);
                    if (it_slot != it->second.end() && it_slot->second >= 2) delta--;
                }
            }
            // Course
            {
                auto it = current_idx.course_slot_section_idx.find(chg.old_a.course_id);
                if (it != current_idx.course_slot_section_idx.end()) {
                     auto it_slot = it->second.find(slot_idx);
                     if (it_slot != it->second.end() && it_slot->second >= 2) delta--;
                }
            }
        });

        // Increase violations for new assignment (using idx_after)
        for_each_slot(chg.new_a, cache, [&](const Slot &slot) {
            int slot_idx = safe_slot_index(slot.day_idx, slot.period_idx, cache.num_periods, "calc_delta:new");
            if (slot_idx < 0) return;
            
            // Teacher
            {
                auto it = ctx.idx_after.teacher_busy_idx.find(chg.new_a.teacher_id);
                if (it != ctx.idx_after.teacher_busy_idx.end()) {
                    auto it_slot = it->second.find(slot_idx);
                    if (it_slot != it->second.end() && it_slot->second >= 2) delta++;
                }
            }
            // Classroom
            if (!chg.new_a.classroom_id.empty()) {
                auto it = ctx.idx_after.classroom_busy_idx.find(chg.new_a.classroom_id);
                if (it != ctx.idx_after.classroom_busy_idx.end()) {
                    auto it_slot = it->second.find(slot_idx);
                    if (it_slot != it->second.end() && it_slot->second >= 2) delta++;
                }
            }
             // Course
            {
                auto it = ctx.idx_after.course_slot_section_idx.find(chg.new_a.course_id);
                if (it != ctx.idx_after.course_slot_section_idx.end()) {
                     auto it_slot = it->second.find(slot_idx);
                     if (it_slot != it->second.end() && it_slot->second >= 2) delta++;
                }
            }
        });
        
        // 2. Local Constraints
        // Old assignment local violations
        bool old_room_exists = lookups.classroom_exists(chg.old_a.classroom_id);
        bool old_room_cap_ok = old_room_exists && (lookups.get_classroom_capacity(chg.old_a.classroom_id) >= lookups.get_required_seats(chg.old_a.course_id, chg.old_a.section_id));
        bool old_eligible = lookups.is_teacher_eligible(chg.old_a.teacher_id, chg.old_a.course_id);
        if (!old_room_exists) delta--;
        if (!old_room_cap_ok) delta--;
        if (!old_eligible) delta--;
        
        // New assignment local violations
        bool new_room_exists = lookups.classroom_exists(chg.new_a.classroom_id);
        bool new_room_cap_ok = new_room_exists && (lookups.get_classroom_capacity(chg.new_a.classroom_id) >= lookups.get_required_seats(chg.new_a.course_id, chg.new_a.section_id));
        bool new_eligible = lookups.is_teacher_eligible(chg.new_a.teacher_id, chg.new_a.course_id);
        if (!new_room_exists) delta++;
        if (!new_room_cap_ok) delta++;
        if (!new_eligible) delta++;
    }

    // 3. Global constraints (Max Courses / Teachers)
    // We strictly check affected entities
    std::unordered_set<std::string> affected_teachers;
    std::unordered_set<std::string> affected_courses;
    
    for (const auto &chg : ctx.changes) {
        affected_teachers.insert(chg.old_a.teacher_id);
        affected_teachers.insert(chg.new_a.teacher_id);
        affected_courses.insert(chg.old_a.course_id);
        affected_courses.insert(chg.new_a.course_id);
    }
    
    for (const auto &try_tid : affected_teachers) {
        const Teacher* teacher = nullptr;
        // Optimization: Use teacher_map if available? Not passed here.
        // Fallback: search in data.teachers. Or assume we can pass teacher_map.
        // Actually, evaluate_move gets teacher_map. But calculate_delta_hard doesn't.
        // I should just search or pass teacher_map. Searching O(T) * affected(2) is fast enough.
        // Or improved: precompute tid -> Teacher const* in MoveEvaluator? No.
        for (const auto &t : data.teachers) { if (t.id == try_tid) { teacher = &t; break; } }
        if (!teacher) continue;
        
        int limit = teacher->max_courses;
        int old_count = 0;
        auto it_old = current_idx.teacher_courses.find(try_tid);
        if (it_old != current_idx.teacher_courses.end()) old_count = (int)it_old->second.size();
        
        int new_count = 0;
        auto it_new = ctx.idx_after.teacher_courses.find(try_tid);
        if (it_new != ctx.idx_after.teacher_courses.end()) new_count = (int)it_new->second.size();
        
        int old_viol = (old_count > limit) ? (old_count - limit) : 0;
        int new_viol = (new_count > limit) ? (new_count - limit) : 0;
        delta += (new_viol - old_viol);
    }
    
    for (const auto &try_cid : affected_courses) {
        const Course* course = nullptr;
        for (const auto &c : data.courses) { if (c.id == try_cid) { course = &c; break; } }
        if (!course) continue;
        
        int min_t = course->min_teachers;
        int max_t = course->max_teachers;
        
        int old_count = 0;
        auto it_old = current_idx.course_teachers.find(try_cid);
        if (it_old != current_idx.course_teachers.end()) old_count = (int)it_old->second.size();
        
        int new_count = 0;
        auto it_new = ctx.idx_after.course_teachers.find(try_cid);
        if (it_new != ctx.idx_after.course_teachers.end()) new_count = (int)it_new->second.size();
        
        int old_viol = 0;
        if (min_t > 0 && old_count < min_t) old_viol += (min_t - old_count);
        if (max_t > 0 && old_count > max_t) old_viol += (old_count - max_t);
        
        int new_viol = 0;
        if (min_t > 0 && new_count < min_t) new_viol += (min_t - new_count);
        if (max_t > 0 && new_count > max_t) new_viol += (new_count - max_t);
        
        delta += (new_viol - old_viol);
    }
    
    return delta;
}

static double get_time_pref_score(const OptimalSolution::Assignment &a,
                                  const ProblemData &data,
                                  const CachedIndices &cache,
                                  const std::unordered_map<std::string, std::unordered_map<int, int>> &time_pref_map) {
    double score = 0.0;
    int required_periods = cache.get_required_periods(a.course_id, a.section_id);
    int start_period_idx = cache.get_period_idx(a.period);
    int day_idx = cache.get_day_idx(a.day);
    int num_periods = cache.num_periods;
    
    if (start_period_idx >= 0 && day_idx >= 0) {
        auto teacher_it = time_pref_map.find(a.teacher_id);
        if (teacher_it != time_pref_map.end()) {
            for (int t = 0; t < required_periods && start_period_idx + t < num_periods; ++t) {
                int period_idx = start_period_idx + t;
                int slot_idx = safe_slot_index(day_idx, period_idx, num_periods, "get_time_pref_score");
                if (slot_idx >= 0) {
                    auto slot_it = teacher_it->second.find(slot_idx);
                    if (slot_it != teacher_it->second.end()) {
                        score += slot_it->second;
                    }
                }
            }
        }
    }
    return score;
}

static double compute_stability_penalty(const OptimalSolution::Assignment &current,
                                       const OptimalSolution::Assignment &initial,
                                       const ProblemData &data) {
    constexpr double STABILITY_TEACHER_PENALTY = 1.5;
    constexpr double STABILITY_TIMESLOT_PENALTY = 1.0;
    constexpr double STABILITY_CORE_MULTIPLIER = 1.5;
    
    double penalty = 0.0;
    
    if (current.teacher_id != initial.teacher_id) {
        penalty += STABILITY_TEACHER_PENALTY;
    }
    
    if (current.day != initial.day || current.period != initial.period) {
        penalty += STABILITY_TIMESLOT_PENALTY;
    }
    
    if (current.teacher_id != initial.teacher_id && 
        (current.day != initial.day || current.period != initial.period)) {
        penalty *= STABILITY_CORE_MULTIPLIER;
    }
    
    return penalty;
}

static double compute_workload_delta(const std::vector<AssignmentChange> &changes,
                                    const PenaltyState &current_state,
                                    const ProblemData &data) {
    PenaltyState temp_state = current_state;
    
    for (const auto &chg : changes) {
        temp_state.update_workload(chg, data);
    }
    
    return temp_state.workload_var - current_state.workload_var;
}

static double compute_compactness_delta(const std::vector<AssignmentChange> &changes,
                                      const PenaltyState &current_state,
                                      const ProblemData &data) {
    PenaltyState temp_state = current_state;
    
    double initial_compactness = current_state.compactness;
    
    for (const auto &chg : changes) {
        temp_state.update_compactness(chg, data);
    }
    
    return temp_state.compactness - initial_compactness;
}

std::pair<double, double> evaluate_move(
    const MoveContext &ctx,
    const OptimalSolution &current,
    const SolIndex &current_idx,
    const PenaltyState &current_state,
    const ProblemData &data,
    const CachedIndices &cache,
    const CachedLookups &lookups,
    const std::unordered_map<std::string, const Teacher*> &teacher_map,
    const std::unordered_map<std::string, std::unordered_map<int, int>> &time_pref_map,
    const std::unordered_map<std::string, OptimalSolution::Assignment> &initial_map,
    const OptimalSolution &initial_sol,
    MoveDelta &delta_out) {
    const double w_course_pref = 1.0, w_time_pref = 1.0, w_workload_balance = 5.0;
    const double w_compactness = 3.0, w_stability = 2.0;
    
    double delta_course_pref = 0.0, delta_time_pref = 0.0, delta_stability = 0.0;
    for (const auto &chg : ctx.changes) {
        auto get_course_pref = [&](const std::string &tid, const std::string &cid) -> double {
            auto it = teacher_map.find(tid);
            if (it == teacher_map.end()) return 0.0;
            auto pc_it = it->second->course_pref.find(cid);
            return (pc_it != it->second->course_pref.end()) ? pc_it->second : 0.0;
        };
        delta_course_pref += get_course_pref(chg.new_a.teacher_id, chg.new_a.course_id) - 
                             get_course_pref(chg.old_a.teacher_id, chg.old_a.course_id);
        delta_time_pref += get_time_pref_score(chg.new_a, data, cache, time_pref_map) -
                           get_time_pref_score(chg.old_a, data, cache, time_pref_map);
        auto it_initial = initial_map.find(get_assignment_id(chg.old_a));
        if (it_initial != initial_map.end())
            delta_stability += compute_stability_penalty(chg.new_a, it_initial->second, data) - 
                               compute_stability_penalty(chg.old_a, it_initial->second, data);
    }
    
    delta_out.delta_workload_var = compute_workload_delta(ctx.changes, current_state, data);
    delta_out.delta_compactness = compute_compactness_delta(ctx.changes, current_state, data);
    
    double current_workload_var = current_state.workload_var;
    double new_workload_var = current_workload_var + delta_out.delta_workload_var;
    int current_num_teachers = (int)current_state.workload.size();
    
    // CachedIndices cache(data); // Removed local cache
    std::unordered_map<std::string, int> new_workload = current_state.workload;
    for (const auto &chg : ctx.changes) {
        int old_p = cache.get_required_periods(chg.old_a.course_id, chg.old_a.section_id);
        int new_p = cache.get_required_periods(chg.new_a.course_id, chg.new_a.section_id);
        new_workload[chg.old_a.teacher_id] -= old_p;
        if (new_workload[chg.old_a.teacher_id] <= 0) new_workload.erase(chg.old_a.teacher_id);
        new_workload[chg.new_a.teacher_id] += new_p;
    }
    int new_num_teachers = (int)new_workload.size();
    
    double current_workload_penalty = (current_num_teachers > 0) ? std::sqrt(current_workload_var / current_num_teachers) : 0.0;
    double new_workload_penalty = (new_num_teachers > 0) ? std::sqrt(new_workload_var / new_num_teachers) : 0.0;
    double delta_workload = new_workload_penalty - current_workload_penalty;
    
    double delta_soft = w_course_pref * delta_course_pref + w_time_pref * delta_time_pref - w_stability * delta_stability;
    double delta = delta_soft - w_workload_balance * delta_workload - w_compactness * delta_out.delta_compactness;
    
    delta_out.delta_soft_local = delta_soft;
    delta_out.delta_hard = calculate_delta_hard(ctx, data, cache, lookups, current_idx);
    
    return {current.objective_value + delta, delta};
}

} // namespace phase3

