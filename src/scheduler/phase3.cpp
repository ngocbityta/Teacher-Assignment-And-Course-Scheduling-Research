#include "phase3.h"
#include "phase3_invariants.h"
#include "phase3_index.h"
#include "phase3_move.h"
#include "phase3_move_builder.h"
#include "phase3_move_evaluator.h"
#include "phase3_move_committer.h"
#include "phase3_penalty_state.h"
#include "phase3_sa.h"
#include "phase3_repair.h"
#include "phase_common.h"
#include <cassert>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <iostream>

using namespace std;
using namespace phase3;

static inline string get_assignment_id(const OptimalSolution::Assignment &a) {
    return a.course_id + "|" + a.section_id;
}

// Assign rooms to assignments that don't have rooms yet
// Uses greedy assignment: try to assign rooms without conflicts, fallback to best-fit
static void assign_initial_rooms(OptimalSolution &sol, const ProblemData &data) {
    if (data.classrooms.classrooms.empty()) {
        // No classrooms available, leave assignments without rooms
        return;
    }
    
    // Build room availability map: (room_id, day_idx, period_idx) -> available
    unordered_map<string, unordered_map<int, unordered_set<int>>> room_busy;
    
    // First pass: assign rooms to assignments that already have rooms (validate them)
    for (auto &a : sol.assignments) {
        if (!a.classroom_id.empty()) {
            // Validate existing room assignment
            int day_idx = find_day_index(data.classrooms.days, a.day);
            int period_idx = find_period_index(data.classrooms.periods, a.period);
            int required_periods = get_required_periods(data, a.course_id, a.section_id);
            
            if (day_idx >= 0 && period_idx >= 0 && period_idx + required_periods <= (int)data.classrooms.periods.size()) {
                // Check if room exists and has sufficient capacity
                bool room_valid = false;
                int required_seats = get_required_seats(data, a.course_id, a.section_id);
                
                for (const auto &room : data.classrooms.classrooms) {
                    if (room.id == a.classroom_id && room.capacity >= required_seats) {
                        room_valid = true;
                        // Mark room as busy for these slots
                        for (int p = 0; p < required_periods; ++p) {
                            room_busy[a.classroom_id][day_idx].insert(period_idx + p);
                        }
                        break;
                    }
                }
                
                if (!room_valid) {
                    // Invalid room assignment, clear it
                    a.classroom_id = "";
                }
            } else {
                // Invalid time slot, clear room
                a.classroom_id = "";
            }
        }
    }
    
    // Sort rooms by capacity (ascending) for better utilization
    vector<pair<int, string>> rooms_sorted; // (capacity, room_id)
    for (const auto &room : data.classrooms.classrooms) {
        rooms_sorted.push_back({room.capacity, room.id});
    }
    sort(rooms_sorted.begin(), rooms_sorted.end());
    
    // Second pass: assign rooms to assignments without rooms
    for (auto &a : sol.assignments) {
        if (a.classroom_id.empty()) {
            int day_idx = find_day_index(data.classrooms.days, a.day);
            int period_idx = find_period_index(data.classrooms.periods, a.period);
            int required_periods = get_required_periods(data, a.course_id, a.section_id);
            int required_seats = get_required_seats(data, a.course_id, a.section_id);
            
            if (day_idx < 0 || period_idx < 0 || period_idx + required_periods > (int)data.classrooms.periods.size()) {
                continue; // Invalid time slot, skip
            }
            
            // Try to find a room without conflict, preferring rooms that are just-right size
            // Score: prefer rooms with capacity close to required_seats (not too large)
            vector<pair<double, pair<int, string>>> room_candidates; // (score, (capacity, room_id))
            
            for (const auto &[capacity, room_id] : rooms_sorted) {
                if (capacity < required_seats) continue; // Room too small
                
                // Check if room is available for all required slots
                bool available = true;
                for (int p = 0; p < required_periods; ++p) {
                    int period_idx_slot = period_idx + p;
                    if (room_busy[room_id][day_idx].count(period_idx_slot)) {
                        available = false;
                        break;
                    }
                }
                
                if (available) {
                    // Score: prefer rooms with capacity close to required_seats
                    // Lower score = better (capacity just above required is best)
                    double size_score = (double)(capacity - required_seats) / std::max(required_seats, 1);
                    // Prefer smaller rooms if same size difference
                    double score = size_score * 100.0 + (capacity < required_seats * 2 ? 0.0 : 50.0);
                    room_candidates.push_back({score, {capacity, room_id}});
                }
            }
            
            // Sort by score (lower is better)
            sort(room_candidates.begin(), room_candidates.end());
            
            bool assigned = false;
            if (!room_candidates.empty()) {
                // Use best-scored room (just-right size, no conflict)
                const auto &[capacity, room_id] = room_candidates[0].second;
                a.classroom_id = room_id;
                // Mark room as busy
                for (int p = 0; p < required_periods; ++p) {
                    room_busy[room_id][day_idx].insert(period_idx + p);
                }
                assigned = true;
            }
            
            if (!assigned) {
                // Fallback: assign ANY room with sufficient capacity (even with conflicts)
                // This ensures all assignments have a room - conflicts will be handled by SA
                for (const auto &[capacity, room_id] : rooms_sorted) {
                    if (capacity >= required_seats) {
                        a.classroom_id = room_id;
                        // Mark room as busy to track conflicts
                        for (int p = 0; p < required_periods; ++p) {
                            room_busy[room_id][day_idx].insert(period_idx + p);
                        }
                        break;
                    }
                }
            }
        }
    }
}
    
// Objective function evaluation
static double Evaluate(const OptimalSolution &sol, const ProblemData &data,
                      const unordered_map<string, const Teacher*> &teacher_map,
                      const unordered_map<string, unordered_map<int, int>> &time_pref_map,
                      const OptimalSolution &initial_sol) {
    const double w_course_pref = 1.0, w_time_pref = 1.0, w_workload_balance = 5.0;
    const double w_compactness = 3.0, w_stability = 2.0;
    
    double course_pref_score = 0.0, time_pref_score = 0.0;
    for (const auto &a : sol.assignments) {
        auto it_teacher = teacher_map.find(a.teacher_id);
        if (it_teacher != teacher_map.end()) {
            auto pc_it = it_teacher->second->course_pref.find(a.course_id);
            if (pc_it != it_teacher->second->course_pref.end()) 
                course_pref_score += pc_it->second;
        }
        
        int required_periods = 1;
        for (const auto &c : data.courses) {
            if (c.id == a.course_id) {
                for (const auto &s : c.sections) {
                    if (s.id == a.section_id) {
                        required_periods = s.required_periods;
                        break;
                    }
                }
                break;
            }
        }
        
        int start_period_idx = find_period_index(data.classrooms.periods, a.period);
        int day_idx = find_day_index(data.classrooms.days, a.day);
        int num_periods = (int)data.classrooms.periods.size();
        if (start_period_idx >= 0 && day_idx >= 0) {
            auto teacher_it = time_pref_map.find(a.teacher_id);
            if (teacher_it != time_pref_map.end()) {
                for (int t = 0; t < required_periods && start_period_idx + t < num_periods; ++t) {
                    int period_idx = start_period_idx + t;
                    int slot_idx_val = slot_index(day_idx, period_idx, num_periods);
                    auto slot_it = teacher_it->second.find(slot_idx_val);
                    if (slot_it != teacher_it->second.end()) {
                        time_pref_score += slot_it->second;
                    }
                }
            }
        }
    }
    
    PenaltyState state = init_penalty_state(sol, data);
    
    unordered_map<string, OptimalSolution::Assignment> initial_map;
    for (const auto &a : initial_sol.assignments) 
        initial_map[get_assignment_id(a)] = a;
    
    double stability_penalty = 0.0;
    const double STABILITY_TEACHER_PENALTY = 1.5;
    const double STABILITY_TIMESLOT_PENALTY = 1.0;
    const double STABILITY_CORE_MULTIPLIER = 1.5;
    
    for (const auto &a : sol.assignments) {
        auto it = initial_map.find(get_assignment_id(a));
        if (it != initial_map.end()) {
            const auto &initial_a = it->second;
            double penalty = 0.0;
            if (a.teacher_id != initial_a.teacher_id) {
                penalty += STABILITY_TEACHER_PENALTY;
            }
            if (a.day != initial_a.day || a.period != initial_a.period) {
                penalty += STABILITY_TIMESLOT_PENALTY;
            }
            if (a.teacher_id != initial_a.teacher_id && 
                (a.day != initial_a.day || a.period != initial_a.period)) {
                penalty *= STABILITY_CORE_MULTIPLIER;
            }
            stability_penalty += penalty;
        }
    }
    
    const double w_room_penalty = 5.0; // Room conflicts and capacity violations (reduced to allow SA flexibility)
    
    return w_course_pref * course_pref_score + w_time_pref * time_pref_score
           - w_workload_balance * state.get_workload_penalty()
           - w_compactness * state.compactness
           - w_stability * stability_penalty
           - w_room_penalty * state.get_room_penalty();
}
    
// Phase 3 initialization data structure
struct Phase3InitData {
    unordered_map<string, const Teacher*> teacher_map;
    unordered_map<string, unordered_map<int, int>> time_pref_map;
    unordered_map<string, OptimalSolution::Assignment> initial_assignment_map;
    OptimalSolution initial_solution;
    OptimalSolution current_solution;
    OptimalSolution best_solution;
    double initial_score;
    SimpleTabu tabu_list;
    SolIndex solution_index;
    PenaltyState penalty_state;
    
    const int num_neighborhoods = 5; // Added ROOM_SWAP and ROOM_SHIFT neighborhoods
    const double ema_beta = 0.1;
    const double weight_min = 0.2;
    const double weight_max = 3.0;
    vector<double> neighborhood_scores;
    vector<double> neighborhood_weights;
    
    double temperature;
    const double initial_temperature = 1.0;
    const double cooling_rate = 0.98;
    
    const int max_iterations = 1200;
    const int base_moves_per_neighborhood = 32;
    const int limit_no_improvement = 220;
    const int stuck_threshold = 100;
    const int intensification_interval = 50;
    
    int iterations_without_improvement = 0;
    int last_intensification_iteration;
    bool just_improved_best_flag = false;
    double last_best_score_value;
};

static Phase3InitData initialize_phase3_search(const ProblemData &data, const InitialSolution &initial) {
    Phase3InitData init_data;
    
    // Build teacher map
    for (const auto &teacher : data.teachers) {
        init_data.teacher_map[teacher.id] = &teacher;
    }
    
    int num_periods = (int)data.classrooms.periods.size();
    for (const auto &teacher : data.teachers) {
        for (const auto &time_pref : teacher.time_pref) {
            int day_idx = find_day_index(data.classrooms.days, time_pref.day);
            int period_idx = find_period_index(data.classrooms.periods, time_pref.period);
            if (day_idx >= 0 && period_idx >= 0) {
                int slot_idx_val = slot_index(day_idx, period_idx, num_periods);
                init_data.time_pref_map[teacher.id][slot_idx_val] = time_pref.score;
            }
        }
    }
    
    init_data.initial_solution = initial;
    init_data.current_solution = initial;
    init_data.best_solution = initial;
    
    assign_initial_rooms(init_data.initial_solution, data);
    assign_initial_rooms(init_data.current_solution, data);
    assign_initial_rooms(init_data.best_solution, data);
    
    init_data.initial_score = Evaluate(init_data.initial_solution, data, 
                                       init_data.teacher_map, init_data.time_pref_map, 
                                       init_data.initial_solution);
    init_data.initial_solution.objective_value = init_data.initial_score;
    init_data.current_solution.objective_value = init_data.initial_score;
    init_data.best_solution.objective_value = init_data.initial_score;
    
    init_data.tabu_list = SimpleTabu(50);
    init_data.solution_index = build_index(init_data.current_solution, data);
    init_data.penalty_state = init_penalty_state(init_data.current_solution, data);
    
    for (const auto &assignment : init_data.initial_solution.assignments) {
        string assignment_id = get_assignment_id(assignment);
        init_data.initial_assignment_map[assignment_id] = assignment;
    }
    
    init_data.neighborhood_scores = {
        (1.2 - init_data.weight_min) / (init_data.weight_max - init_data.weight_min), // SINGLE_CHANGE
        (0.8 - init_data.weight_min) / (init_data.weight_max - init_data.weight_min), // BLOCK_RELOCATE
        (0.5 - init_data.weight_min) / (init_data.weight_max - init_data.weight_min), // CHAIN_MOVE
        (0.7 - init_data.weight_min) / (init_data.weight_max - init_data.weight_min), // ROOM moves
        (0.6 - init_data.weight_min) / (init_data.weight_max - init_data.weight_min)  // MULTI_SWAP
    };
    init_data.neighborhood_weights.resize(init_data.num_neighborhoods);
    for (int i = 0; i < init_data.num_neighborhoods; ++i) {
        init_data.neighborhood_weights[i] = init_data.weight_min + 
            (init_data.weight_max - init_data.weight_min) * init_data.neighborhood_scores[i];
    }
    
    init_data.temperature = init_data.initial_temperature;
    init_data.last_best_score_value = init_data.best_solution.objective_value;
    init_data.last_intensification_iteration = -init_data.intensification_interval;
    
    return init_data;
}

static void log_phase3_results(const Phase3InitData &init_data) {
    // Output final objective value for evaluation (required by interface)
    cout << init_data.best_solution.objective_value << endl;
}

OptimalSolution find_optimal_solution(const ProblemData &data, const InitialSolution &initial) {
    Phase3InitData init_data = initialize_phase3_search(data, initial);
    
    // CRITICAL: Repair constraint violations BEFORE SA optimization
    // This fixes classroom conflicts and max_teachers violations from Phase 2
    std::cout << "[PHASE3] [REPAIR] Checking initial solution for constraint violations..." << std::endl;
    int initial_violations = count_hard_violations(init_data.current_solution, data);
    
    if (initial_violations > 0) {
        std::cout << "[PHASE3] [REPAIR] Found " << initial_violations 
                  << " violations. Running repair phase..." << std::endl;
        
        init_data.current_solution = final_repair_solution(init_data.current_solution, data);
        init_data.best_solution = init_data.current_solution;
        
        // Rebuild indices after repair
        init_data.solution_index = build_index(init_data.current_solution, data);
        init_data.penalty_state = init_penalty_state(init_data.current_solution, data);
        
        // Recalculate objective after repair
        init_data.current_solution.objective_value = Evaluate(
            init_data.current_solution, data, init_data.teacher_map, 
            init_data.time_pref_map, init_data.initial_solution);
        init_data.best_solution.objective_value = init_data.current_solution.objective_value;
        
        int repaired_violations = count_hard_violations(init_data.current_solution, data);
        std::cout << "[PHASE3] [REPAIR] After repair: " << repaired_violations 
                  << " violations remaining" << std::endl;
    } else {
        std::cout << "[PHASE3] [REPAIR] No violations found. Skipping repair phase." << std::endl;
    }
    
    // Use the new modular SA implementation
    run_simulated_annealing(
        init_data.current_solution,
        init_data.best_solution,
        init_data.solution_index,
        init_data.penalty_state,
        init_data.tabu_list,
        data,
        init_data.initial_solution,
        init_data.teacher_map,
        init_data.time_pref_map,
        init_data.initial_assignment_map);
    
    
    // BUG FIX: Don't recalculate objective - it's already correct from SA
    // Recalculating can cause discrepancies due to incremental vs full recalculation
    // The objective_value in best_solution is already set correctly during commit_move()
    // init_data.best_solution.objective_value = Evaluate(
    //     init_data.best_solution, data, init_data.teacher_map, init_data.time_pref_map, 
    //     init_data.initial_solution);
    
    
    // Validate constraints (only log errors to cerr if violations found, similar to CPSatSchedule)
    bool is_valid = check_hard_invariant(init_data.best_solution, data);
    int violations = count_hard_violations(init_data.best_solution, data);
    
    if (!is_valid || violations > 0) {
        cerr << "[PHASE3 VALIDATION ERROR] Solution violates constraints!" << endl;
        cerr << "[PHASE3 VALIDATION] Violations count: " << violations << endl;
        cerr << "[PHASE3 VALIDATION] Solution is_valid: " << (is_valid ? "true" : "false") << endl;
        cerr << endl << "=== DETAILED CONSTRAINT VIOLATIONS ===" << endl;
        report_constraint_violations(init_data.best_solution, data);
        
        // Fall back to the initial solution which is guaranteed to be valid from Phase 2
        init_data.best_solution = init_data.initial_solution;
        init_data.best_solution.objective_value = Evaluate(
            init_data.best_solution, data, init_data.teacher_map, init_data.time_pref_map, 
            init_data.initial_solution);
    }
    
    // Output final objective value (same format as CPSatSchedule)
    cout << init_data.best_solution.objective_value << endl;
    
    return init_data.best_solution;
}

double evaluate_initial_solution(const ProblemData& data, const InitialSolution& init_sol) {
    // Build teacher and time preference maps for evaluation
    unordered_map<string, const Teacher*> teacher_map;
    for (const auto &t : data.teachers) {
        teacher_map[t.id] = &t;
    }
    
    unordered_map<string, unordered_map<int, int>> time_pref_map;
    int num_periods = (int)data.classrooms.periods.size();
    for (const auto &t : data.teachers) {
        for (const auto &tp : t.time_pref) {
            int day_idx = find_day_index(data.classrooms.days, tp.day);
            int period_idx = find_period_index(data.classrooms.periods, tp.period);
            if (day_idx >= 0 && period_idx >= 0) {
                int slot_idx_val = slot_index(day_idx, period_idx, num_periods);
                time_pref_map[t.id][slot_idx_val] = tp.score;
            }
        }
    }
    
    OptimalSolution opt_sol(init_sol);
    return Evaluate(opt_sol, data, teacher_map, time_pref_map, opt_sol);
}

