#include "phase3_move_builder.h"
#include "phase3_invariants.h"
#include <random>
#include <chrono>
#include <algorithm>
#include <numeric>

namespace phase3 {

constexpr int MIN_CHAIN_SIZE = 3;
constexpr int TOP_K_TEACHERS = 3;
constexpr double COMPACTNESS_WEIGHT = 2.0;

std::mt19937 MoveBuilder::rng(std::chrono::steady_clock::now().time_since_epoch().count());

static const Course* find_course(const ProblemData &data, const std::string &course_id) {
    for (const auto &c : data.courses)
        if (c.id == course_id) return &c;
    return nullptr;
}

int MoveBuilder::select_random_assignment(const OptimalSolution &sol) {
    if (sol.assignments.empty()) return -1;
    std::uniform_int_distribution<int> dist(0, (int)sol.assignments.size() - 1);
    return dist(rng);
}

// Weighted random selection based on penalty scores
template<typename T>
static T select_weighted_random(const std::vector<std::pair<T, double>> &weighted_items, std::mt19937 &rng) {
    if (weighted_items.empty()) return T();
    
    // Calculate total weight
    double total_weight = 0.0;
    for (const auto &[item, weight] : weighted_items) {
        total_weight += std::max(0.0, weight); // Only positive weights
    }
    
    if (total_weight <= 0.0) {
        // Fallback to uniform random if all weights are non-positive
        std::uniform_int_distribution<int> dist(0, (int)weighted_items.size() - 1);
        return weighted_items[dist(rng)].first;
    }
    
    // Select based on weighted probability
    std::uniform_real_distribution<double> dist(0.0, total_weight);
    double random_value = dist(rng);
    double cumulative = 0.0;
    
    for (const auto &[item, weight] : weighted_items) {
        cumulative += std::max(0.0, weight);
        if (random_value <= cumulative) {
            return item;
        }
    }
    
    // Fallback (shouldn't reach here)
    return weighted_items.back().first;
}

std::pair<std::string, std::string> MoveBuilder::select_random_timeslot(const ProblemData &data) {
    std::uniform_int_distribution<int> ldist(0, (int)data.classrooms.days.size() - 1);
    std::uniform_int_distribution<int> pdist(0, (int)data.classrooms.periods.size() - 1);
    return {data.classrooms.days[ldist(rng)], data.classrooms.periods[pdist(rng)]};
}

// Weighted random timeslot selection based on penalty
std::pair<std::string, std::string> MoveBuilder::select_weighted_timeslot(
    const ProblemData &data, 
    const PenaltyState &penalty_state,
    const std::string &teacher_id) {
    
    // Build weighted list of timeslots based on compactness penalty
    std::vector<std::pair<std::pair<std::string, std::string>, double>> weighted_slots;
    
    auto day_slots_it = penalty_state.day_slots.find(teacher_id);
    for (const auto &day : data.classrooms.days) {
        for (const auto &period : data.classrooms.periods) {
            double weight = 1.0; // Default weight
            
            if (day_slots_it != penalty_state.day_slots.end()) {
                auto day_it = day_slots_it->second.find(day);
                if (day_it != day_slots_it->second.end()) {
                    // Higher penalty = higher weight (more likely to be selected for improvement)
                    double compactness = PenaltyState::compute_compactness_for_set(day_it->second);
                    weight = 1.0 + compactness * COMPACTNESS_WEIGHT;
                }
            }
            
            weighted_slots.push_back({{day, period}, weight});
        }
    }
    
    return select_weighted_random(weighted_slots, rng);
}

std::string MoveBuilder::select_random_teacher(const std::vector<std::string> &teachers) {
    if (teachers.empty()) return "";
    std::uniform_int_distribution<int> tdist(0, (int)teachers.size() - 1);
    return teachers[tdist(rng)];
}

std::vector<Block> MoveBuilder::extract_chain(const std::string &teacher_id, const std::string &day, 
                                             const OptimalSolution &sol, const ProblemData &data) {
    CachedIndices cache(data);
    std::vector<Block> chain;
    
    for (int assignment_idx = 0; assignment_idx < (int)sol.assignments.size(); ++assignment_idx) {
        const auto &assignment = sol.assignments[assignment_idx];
        if (assignment.teacher_id == teacher_id && assignment.day == day) {
            int required_periods = cache.get_required_periods(assignment.course_id, assignment.section_id);
            chain.push_back({assignment_idx, assignment.period, required_periods});
        }
    }
    
    std::sort(chain.begin(), chain.end(), [&](const Block &block_a, const Block &block_b) {
        int period_idx_a = cache.get_period_idx(block_a.period);
        int period_idx_b = cache.get_period_idx(block_b.period);
        return period_idx_a < period_idx_b;
    });
    
    if (chain.size() < MIN_CHAIN_SIZE) return {};
    
    int common_required_periods = chain[0].required_periods;
    for (const auto &block : chain) {
        if (block.required_periods != common_required_periods) return {};
    }
    
    for (size_t i = 1; i < chain.size(); ++i) {
        int prev_period_idx = cache.get_period_idx(chain[i-1].period);
        int curr_period_idx = cache.get_period_idx(chain[i].period);
        
        if (prev_period_idx < 0 || curr_period_idx < 0) return {};
        
        int expected_curr_period_idx = prev_period_idx + common_required_periods;
        if (curr_period_idx != expected_curr_period_idx) return {};
    }
    
    return chain;
}

void MoveBuilder::rotate_chain(std::vector<Block> &chain, const ProblemData &data) {
    if (chain.empty()) return;
    
    CachedIndices cache(data);
    int common_required_periods = chain[0].required_periods;
    
    std::vector<std::string> original_periods(chain.size());
    for (size_t i = 0; i < chain.size(); ++i) {
        original_periods[i] = chain[i].period;
    }
    
    // Try to find a valid sub-chain if full rotation fails
    for (int start_size = (int)chain.size(); start_size >= MIN_CHAIN_SIZE; --start_size) {
        std::vector<Block> trial_chain(chain.begin(), chain.begin() + start_size);
        std::vector<std::string> trial_periods(original_periods.begin(), original_periods.begin() + start_size);
        bool valid = true;
        
        for (size_t i = 0; i < trial_chain.size(); ++i) {
            size_t prev_block_idx = (i - 1 + trial_chain.size()) % trial_chain.size();
            int new_start_period_idx = cache.get_period_idx(trial_periods[prev_block_idx]);
            
            if (new_start_period_idx < 0 || new_start_period_idx + common_required_periods > cache.num_periods) {
                valid = false;
                break;
            }
            
            trial_chain[i].period = data.classrooms.periods[new_start_period_idx];
        }
        
        if (valid) {
            chain = trial_chain;
            return;
        }
    }
    
    // If no valid sub-chain found, clear
    chain.clear();
}

MoveBuilder::MoveBuilder(const OptimalSolution &sol_ref, const ProblemData &data_ref) 
    : sol(sol_ref), data(data_ref) { 
    spec = MoveSpec(); 
}

bool MoveBuilder::build_single_change() {
    int idx_assign = select_random_assignment(sol);
    if (idx_assign < 0) return false;
    const auto &a = sol.assignments[idx_assign];
    spec.assignment_indices = {idx_assign};
    spec.type = Move::SINGLE_CHANGE;
    const Course *course = find_course(data, a.course_id);
    if (!course || course->Ij.empty()) return false;
    
    std::string new_teacher = select_random_teacher(course->Ij);
    // Verify eligibility (double-check)
    if (!new_teacher.empty() && !is_teacher_eligible(data, new_teacher, a.course_id)) {
        return false; // Skip move if teacher is not eligible
    }
    
    if (new_teacher != a.teacher_id) {
        spec.new_teacher_id = new_teacher;
        return true;
    }
    auto [new_day, new_period] = select_random_timeslot(data);
    spec.new_day = new_day;
    spec.new_period = new_period;
    return true;
}

bool MoveBuilder::build_block_relocate() {
    int assignment_idx = select_random_assignment(sol);
    if (assignment_idx < 0) return false;
    
    spec.assignment_indices = {assignment_idx};
    spec.type = Move::BLOCK_RELOCATE;
    
    auto [new_day, new_period] = select_random_timeslot(data);
    spec.new_day = new_day;
    spec.new_period = new_period;
    
    return true;
}

bool MoveBuilder::build_chain_move(const PenaltyState &penalty_state) {
    if (data.teachers.empty() || data.classrooms.days.empty()) return false;
    
    spec.type = Move::CHAIN_MOVE;
    std::vector<std::pair<std::string, double>> teacher_penalty_scores;
    
    double mean_workload = 0.0;
    if (!penalty_state.workload.empty()) {
        double total_workload = 0.0;
        for (const auto &[teacher_id, workload] : penalty_state.workload) {
            total_workload += workload;
        }
        mean_workload = total_workload / penalty_state.workload.size();
    }
    
    for (const auto &teacher : data.teachers) {
        double penalty_score = 0.0;
        
        auto workload_it = penalty_state.workload.find(teacher.id);
        if (workload_it != penalty_state.workload.end()) {
            double workload_deviation = std::abs(workload_it->second - mean_workload);
            penalty_score += workload_deviation;
        }
        
        auto day_slots_it = penalty_state.day_slots.find(teacher.id);
        if (day_slots_it != penalty_state.day_slots.end()) {
            for (const auto &[day, period_slots] : day_slots_it->second) {
                double compactness_penalty = PenaltyState::compute_compactness_for_set(period_slots);
                penalty_score += compactness_penalty * COMPACTNESS_WEIGHT;
            }
        }
        
        // Always include teacher, even with 0 penalty (for uniform fallback)
        teacher_penalty_scores.push_back({teacher.id, std::max(0.1, penalty_score)});
    }
    
    // Use weighted random selection instead of top-k
    std::string selected_teacher_id = select_weighted_random(teacher_penalty_scores, rng);
    
    // Weighted day selection based on teacher's day penalties
    std::vector<std::pair<std::string, double>> day_penalty_scores;
    auto day_slots_it = penalty_state.day_slots.find(selected_teacher_id);
    for (const auto &day : data.classrooms.days) {
        double day_penalty = 0.1; // Default weight
        if (day_slots_it != penalty_state.day_slots.end()) {
            auto day_it = day_slots_it->second.find(day);
            if (day_it != day_slots_it->second.end()) {
                double compactness = PenaltyState::compute_compactness_for_set(day_it->second);
                day_penalty = 1.0 + compactness * COMPACTNESS_WEIGHT;
            }
        }
        day_penalty_scores.push_back({day, day_penalty});
    }
    
    std::string selected_day = select_weighted_random(day_penalty_scores, rng);
    
    spec.chain = extract_chain(selected_teacher_id, selected_day, sol, data);
    if (spec.chain.size() < MIN_CHAIN_SIZE) return false;
    
    rotate_chain(spec.chain, data);
    if (spec.chain.empty()) return false;
    
    spec.assignment_indices.clear();
    for (const auto &block : spec.chain) {
        spec.assignment_indices.push_back(block.assignment_idx);
    }
    
    return true;
}

bool MoveBuilder::build_multi_swap(int num_swaps) {
    if (sol.assignments.size() < num_swaps || num_swaps < 2) return false;
    
    spec.type = Move::MULTI_SWAP;
    spec.assignment_indices.clear();
    
    // Select multiple distinct assignments
    std::vector<int> available_indices;
    for (int i = 0; i < (int)sol.assignments.size(); ++i) {
        available_indices.push_back(i);
    }
    std::shuffle(available_indices.begin(), available_indices.end(), rng);
    
    // Take first num_swaps assignments
    int actual_swaps = std::min(num_swaps, (int)available_indices.size());
    for (int i = 0; i < actual_swaps; ++i) {
        spec.assignment_indices.push_back(available_indices[i]);
    }
    
    // Collect current timeslots
    std::vector<std::pair<std::string, std::string>> timeslots;
    for (int idx : spec.assignment_indices) {
        timeslots.push_back({sol.assignments[idx].day, sol.assignments[idx].period});
    }
    
    // Shuffle the timeslots among selected assignments (circular shift or random shuffle)
    std::shuffle(timeslots.begin(), timeslots.end(), rng);
    
    // Store shuffled timeslots
    spec.new_days.clear();
    spec.chain.clear();
    for (size_t i = 0; i < spec.assignment_indices.size(); ++i) {
        spec.new_days.push_back(timeslots[i].first);
        spec.chain.push_back({
            spec.assignment_indices[i],
            timeslots[i].second, // new period
            1 // required_periods (will be looked up during evaluation)
        });
    }
    
    return true;
}

bool MoveBuilder::build_room_swap() {
    // Find two assignments with different rooms that can be swapped
    // Prefer swapping between different days but same period (slot) to reduce conflicts
    if (sol.assignments.size() < 2) return false;
    
    // Collect assignments with rooms, grouped by period
    std::unordered_map<std::string, std::vector<int>> period_to_assignments; // period -> [assignment_idx]
    std::vector<int> all_assignments_with_rooms;
    
    for (int i = 0; i < (int)sol.assignments.size(); ++i) {
        const auto &a = sol.assignments[i];
        if (!a.classroom_id.empty()) {
            all_assignments_with_rooms.push_back(i);
            std::string period_key = a.period; // Same period, different days
            period_to_assignments[period_key].push_back(i);
        }
    }
    
    if (all_assignments_with_rooms.size() < 2) return false;
    
    int idx1 = -1, idx2 = -1;
    
    // Strategy 1: Try to find assignments with same period but different days (better for conflict reduction)
    for (const auto &[period, indices] : period_to_assignments) {
        if (indices.size() >= 2) {
            std::vector<int> candidates;
            for (int idx : indices) {
                candidates.push_back(idx);
            }
            std::shuffle(candidates.begin(), candidates.end(), rng);
            
            // Find two with different days and different rooms
            for (size_t i = 0; i < candidates.size(); ++i) {
                for (size_t j = i + 1; j < candidates.size(); ++j) {
                    int c1 = candidates[i];
                    int c2 = candidates[j];
                    if (sol.assignments[c1].day != sol.assignments[c2].day &&
                        sol.assignments[c1].classroom_id != sol.assignments[c2].classroom_id) {
                        idx1 = c1;
                        idx2 = c2;
                        break;
                    }
                }
                if (idx1 >= 0) break;
            }
            if (idx1 >= 0) break;
        }
    }
    
    // Strategy 2: Fallback to any two assignments with different rooms
    if (idx1 < 0) {
        std::shuffle(all_assignments_with_rooms.begin(), all_assignments_with_rooms.end(), rng);
        for (size_t i = 0; i < all_assignments_with_rooms.size(); ++i) {
            for (size_t j = i + 1; j < all_assignments_with_rooms.size(); ++j) {
                int c1 = all_assignments_with_rooms[i];
                int c2 = all_assignments_with_rooms[j];
                if (sol.assignments[c1].classroom_id != sol.assignments[c2].classroom_id) {
                    idx1 = c1;
                    idx2 = c2;
                    break;
                }
            }
            if (idx1 >= 0) break;
        }
    }
    
    if (idx1 < 0 || idx2 < 0) return false;
    
    spec.type = Move::ROOM_SWAP;
    spec.assignment_indices = {idx1, idx2};
    spec.new_room_ids = {
        sol.assignments[idx2].classroom_id,  // room1 gets room2
        sol.assignments[idx1].classroom_id   // room2 gets room1
    };
    
    return true;
}

bool MoveBuilder::build_room_shift(const PenaltyState &penalty_state) {
    // Find an assignment with a room that could be improved
    if (sol.assignments.empty() || data.classrooms.classrooms.empty()) return false;
    
    // Select a random assignment with a room
    std::vector<int> candidates;
    for (int i = 0; i < (int)sol.assignments.size(); ++i) {
        const auto &a = sol.assignments[i];
        if (!a.classroom_id.empty()) {
            candidates.push_back(i);
        }
    }
    
    if (candidates.empty()) return false;
    
    std::uniform_int_distribution<int> dist(0, (int)candidates.size() - 1);
    int assignment_idx = candidates[dist(rng)];
    const auto &assignment = sol.assignments[assignment_idx];
    
    // Find required seats for this assignment
    int required_seats = 0;
    for (const auto &c : data.courses) {
        if (c.id == assignment.course_id) {
            for (const auto &s : c.sections) {
                if (s.id == assignment.section_id) {
                    required_seats = s.required_seats;
                    break;
                }
            }
            break;
        }
    }
    
    if (required_seats == 0) return false;
    
    // Find current room capacity
    int current_capacity = 0;
    for (const auto &room : data.classrooms.classrooms) {
        if (room.id == assignment.classroom_id) {
            current_capacity = room.capacity;
            break;
        }
    }
    
    // Try to find better rooms - collect ALL suitable rooms with weighted scoring
    std::vector<std::pair<std::string, double>> room_candidates; // (room_id, weight) - higher weight = better
    
    for (const auto &room : data.classrooms.classrooms) {
        if (room.id == assignment.classroom_id) continue; // Skip current room
        
        if (room.capacity < required_seats) continue; // Room too small
        
        // Weight calculation: prefer rooms with capacity close to required_seats
        double size_ratio = (double)room.capacity / std::max(required_seats, 1);
        double weight = 1.0;
        
        // Best: capacity just above required (1.0 < ratio <= 1.5)
        if (size_ratio <= 1.5) {
            weight = 10.0; // Highest weight
        }
        // Good: capacity reasonable (1.5 < ratio <= 2.0)
        else if (size_ratio <= 2.0) {
            weight = 5.0;
        }
        // Acceptable: capacity larger but not too large (2.0 < ratio <= 3.0)
        else if (size_ratio <= 3.0) {
            weight = 2.0;
        }
        // Less preferred: very large room (ratio > 3.0)
        else {
            weight = 0.5;
        }
        
        // Bonus if it's an improvement (smaller room that still fits)
        if (room.capacity < current_capacity && room.capacity >= required_seats) {
            weight *= 1.5; // Boost improvement moves
        }
        
        // Small penalty if it's larger than current (unless current is too small)
        if (room.capacity > current_capacity && current_capacity >= required_seats) {
            weight *= 0.8;
        }
        
        room_candidates.push_back({room.id, weight});
    }
    
    if (room_candidates.empty()) return false;
    
    // Use weighted random selection from ALL suitable rooms (not just top-K)
    // This helps SA escape local minima by exploring more diverse moves
    std::string new_room_id = select_weighted_random(room_candidates, rng);
    
    spec.type = Move::ROOM_SHIFT;
    spec.assignment_indices = {assignment_idx};
    spec.new_room_ids = {new_room_id};
    
    return true;
}

bool MoveBuilder::build(Move::Type move_type, const PenaltyState &state) {
    switch (move_type) {
        case Move::SINGLE_CHANGE: return build_single_change();
        case Move::BLOCK_RELOCATE: return build_block_relocate();
        case Move::CHAIN_MOVE: return build_chain_move(state);
        case Move::MULTI_SWAP: return build_multi_swap(2);
        case Move::ROOM_SWAP: return build_room_swap();
        case Move::ROOM_SHIFT: return build_room_shift(state);
        default: return false;
    }
}

} // namespace phase3

