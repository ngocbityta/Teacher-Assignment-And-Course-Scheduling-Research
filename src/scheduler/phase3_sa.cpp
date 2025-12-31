#include "phase3_sa.h"
#include "phase3_move_builder.h"
#include "phase3_move_evaluator.h"
#include "phase3_move_committer.h"
#include "phase3_index.h"
#include "phase3_penalty_state.h"
#include "phase3_invariants.h"
#include "phase_common.h"
#include <random>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <functional>
#include <sstream>
#include <iomanip>
#include <iostream>

namespace phase3 {

constexpr double MAX_EXPONENT = 700.0;
constexpr double MIN_EXPONENT = -700.0;
constexpr double MIN_TEMPERATURE = 1e-4;
constexpr int DEFAULT_TABU_TENURE = 50;

static inline std::string get_assignment_id(const OptimalSolution::Assignment &a) {
    return a.course_id + "|" + a.section_id;
}

static std::mt19937 rng(std::chrono::steady_clock::now().time_since_epoch().count());

SimpleTabu::SimpleTabu(size_t tenure_val) : tenure(tenure_val) {}

bool SimpleTabu::is_tabu(const std::string &sig_teacher, const std::string &sig_timeslot, const std::string &sig_chain_teacher_day) const {
    return (!sig_teacher.empty() && tabu_set_teacher.count(sig_teacher)) ||
           (!sig_timeslot.empty() && tabu_set_timeslot.count(sig_timeslot)) ||
           (!sig_chain_teacher_day.empty() && tabu_set_chain_teacher_day.count(sig_chain_teacher_day));
}

void SimpleTabu::add_tabu(const std::string &sig_teacher, const std::string &sig_timeslot, const std::string &sig_chain_teacher_day) {
    auto add = [&](std::deque<std::string> &q, std::unordered_set<std::string> &s, const std::string &val) {
        if (!val.empty()) {
            q.push_back(val);
            s.insert(val);
            if (q.size() > tenure) {
                s.erase(q.front());
                q.pop_front();
            }
        }
    };
    add(tabu_queue_teacher, tabu_set_teacher, sig_teacher);
    add(tabu_queue_timeslot, tabu_set_timeslot, sig_timeslot);
    add(tabu_queue_chain_teacher_day, tabu_set_chain_teacher_day, sig_chain_teacher_day);
}

bool should_accept_move(double objective_delta, double temperature, int &sa_rejected,
                       double best_score, double candidate_score, bool is_at_best) {
    // Always accept improving moves
    if (objective_delta >= 0.0) return true;
    
    if (temperature < MIN_TEMPERATURE) {
        sa_rejected++;
        return false;
    }
    
    double log_prob = objective_delta / temperature;
    
    if (log_prob > MAX_EXPONENT) {
        return true;
    }
    if (log_prob < MIN_EXPONENT) {
        sa_rejected++;
        return false;
    }
    
    std::uniform_real_distribution<double> random_uniform(0.0, 1.0);
    double acceptance_probability = std::exp(log_prob);
    
    if (acceptance_probability <= 0.0 || !std::isfinite(acceptance_probability)) {
        sa_rejected++;
        return false;
    }
    
    if (random_uniform(rng) < acceptance_probability) {
        return true;
    }
    
    sa_rejected++;
    return false;
}

std::tuple<std::string, std::string, std::string> extract_tabu_keys(
    const OptimalSolution &old_sol,
    const OptimalSolution &new_sol,
    const Move &move) {
    std::string sig_teacher = "", sig_timeslot = "", sig_chain_teacher_day = "";
    auto check_idx = [&](int idx) -> const OptimalSolution::Assignment* {
        if (idx < 0 || idx >= (int)old_sol.assignments.size() || idx >= (int)new_sol.assignments.size()) return nullptr;
        const auto &old_a = old_sol.assignments[idx], &new_a = new_sol.assignments[idx];
        if (old_a.course_id != new_a.course_id || old_a.section_id != new_a.section_id) return nullptr;
        return &old_a;
    };
    if (move.indices.empty()) return std::make_tuple("", "", "");
    if (move.type == Move::SINGLE_CHANGE || move.type == Move::BLOCK_RELOCATE) {
        const auto *old_a = check_idx(move.indices[0]);
        if (!old_a) return std::make_tuple("", "", "");
        const auto &new_a = new_sol.assignments[move.indices[0]];
        std::string aid = get_assignment_id(*old_a);
        if (old_a->teacher_id != new_a.teacher_id)
            sig_teacher = aid + "|" + old_a->teacher_id + "->" + new_a.teacher_id;
        if (old_a->day != new_a.day || old_a->period != new_a.period) {
            std::string old_slot = old_a->day + "|" + old_a->period, new_slot = new_a.day + "|" + new_a.period;
            sig_timeslot = aid + "|" + std::min(old_slot, new_slot) + "|" + std::max(old_slot, new_slot);
        }
    } else if (move.type == Move::CHAIN_MOVE) {
        if (!move.chain.empty()) {
            const auto &first_block = move.chain[0];
            if (first_block.assignment_idx >= 0 && 
                first_block.assignment_idx < (int)old_sol.assignments.size() &&
                first_block.assignment_idx < (int)new_sol.assignments.size()) {
                const auto &a = old_sol.assignments[first_block.assignment_idx];
                sig_chain_teacher_day = a.teacher_id + "|" + a.day + "|CHAIN";
            } else {
                return std::make_tuple("", "", "");
            }
        }
        for (const auto &block : move.chain) {
            if (block.assignment_idx < 0 || 
                block.assignment_idx >= (int)old_sol.assignments.size() ||
                block.assignment_idx >= (int)new_sol.assignments.size()) {
                continue;
            }
            const auto *old_a = check_idx(block.assignment_idx);
            if (!old_a) continue;
            const auto &new_a = new_sol.assignments[block.assignment_idx];
            if (old_a->day != new_a.day || old_a->period != new_a.period) {
                if (!sig_timeslot.empty()) sig_timeslot += ";";
                std::string aid = get_assignment_id(*old_a);
                std::string old_slot = old_a->day + "|" + old_a->period, new_slot = new_a.day + "|" + new_a.period;
                sig_timeslot += aid + "|" + std::min(old_slot, new_slot) + "|" + std::max(old_slot, new_slot);
            }
        }
    } else if (move.type == Move::MULTI_SWAP) {
        for (int idx : move.indices) {
            const auto *old_a = check_idx(idx);
            if (!old_a) continue;
            const auto &new_a = new_sol.assignments[idx];
            if (old_a->day != new_a.day || old_a->period != new_a.period) {
                if (!sig_timeslot.empty()) sig_timeslot += ";";
                std::string aid = get_assignment_id(*old_a);
                std::string old_slot = old_a->day + "|" + old_a->period, new_slot = new_a.day + "|" + new_a.period;
                sig_timeslot += aid + "|" + std::min(old_slot, new_slot) + "|" + std::max(old_slot, new_slot);
            }
        }
    }
    return std::make_tuple(sig_teacher, sig_timeslot, sig_chain_teacher_day);
}

AcceptMoveResult accept_move(
    const Move &move,
    const OptimalSolution &current_solution,
    const OptimalSolution &candidate_solution,
    double candidate_score,
    double objective_delta,
    double temperature,
    SimpleTabu &tabu_list,
    double best_score,
    int &tabu_rejected_count,
    int &sa_rejected_count) {
    AcceptMoveResult result{false, false, false, "", "", ""};
    
    auto tabu_keys = extract_tabu_keys(current_solution, candidate_solution, move);
    result.sig_teacher = std::get<0>(tabu_keys);
    result.sig_timeslot = std::get<1>(tabu_keys);
    result.sig_chain_teacher_day = std::get<2>(tabu_keys);
    
    bool is_tabu_move = tabu_list.is_tabu(result.sig_teacher, result.sig_timeslot, result.sig_chain_teacher_day);
    
    bool aspiration_criterion = candidate_score > best_score;
    
    if (is_tabu_move && !aspiration_criterion) {
        result.rejected_by_tabu = true;
        tabu_rejected_count++;
        return result;
    }
    
    const double BEST_TOLERANCE = 0.01;
    bool is_current_at_best = (current_solution.objective_value >= best_score - BEST_TOLERANCE);
    
    bool sa_accepted = should_accept_move(objective_delta, temperature, sa_rejected_count,
                                         best_score, candidate_score, is_current_at_best);
    
    if (sa_accepted) {
        tabu_list.add_tabu(result.sig_teacher, result.sig_timeslot, result.sig_chain_teacher_day);
        result.accepted = true;
    } else {
        result.rejected_by_sa = true;
    }
    
    return result;
}

static inline void update_ema_weight(int nb, double reward, double beta, double w_min, double w_max,
                                    std::vector<double> &nb_score, std::vector<double> &nb_weight) {
    nb_score[nb] = beta * reward + (1.0 - beta) * nb_score[nb];
    nb_weight[nb] = w_min + (w_max - w_min) * nb_score[nb];
}

void run_simulated_annealing(
    OptimalSolution &current_solution,
    OptimalSolution &best_solution,
    SolIndex &solution_index,
    PenaltyState &penalty_state,
    SimpleTabu &tabu_list,
    const ProblemData &data,
    const OptimalSolution &initial_solution,
    const std::unordered_map<std::string, const Teacher*> &teacher_map,
    const std::unordered_map<std::string, std::unordered_map<int, int>> &time_pref_map,
    const std::unordered_map<std::string, OptimalSolution::Assignment> &initial_map) {
    
    // Initialize caches once
    CachedIndices cache(data);
    CachedLookups lookups(data);
    
    const int problem_size = (int)current_solution.assignments.size();
    
    int max_iterations = 1200;
    int base_moves_per_neighborhood = 32;
    int limit_no_improvement = 220;
    int stuck_threshold = 100;
    
    if (problem_size > 200) {
        max_iterations = 300;  // Reduced from 400
        base_moves_per_neighborhood = 4;  // Further reduced from 8 (87.5% reduction)
        stuck_threshold = 40;  // Reduced from 50 for faster stuck detection
        limit_no_improvement = 80;  // Reduced from 100
    } else if (problem_size > 100) {
        // Large problem: moderate reduction  
        max_iterations = 800;
        base_moves_per_neighborhood = 16;  // 50% reduction
        stuck_threshold = 75;
        limit_no_improvement = 150;
    } else {
        max_iterations = 500;
        base_moves_per_neighborhood = 8;
        stuck_threshold = 50;
        limit_no_improvement = 100;
    }
    
    const int intensification_interval = 50;
    
    double temperature = 1.0;
    const double initial_temperature = 1.0;
    const double cooling_rate = 0.98;
    
    int iterations_without_improvement = 0;
    int stuck_iterations_count = 0;
    int tabu_rejected_count = 0;
    int sa_rejected_count = 0;
    int infeasible_moves_count = 0;
    int total_moves_accepted = 0;
    int total_moves_attempted = 0;
    
    double initial_best_score = best_solution.objective_value;
    
    const int num_neighborhoods = 5; // Added ROOM_SWAP and ROOM_SHIFT neighborhoods
    const double ema_beta = 0.1;
    const double weight_min = 0.2;
    const double weight_max = 3.0;
    std::vector<double> neighborhood_scores = {
        (1.2 - weight_min) / (weight_max - weight_min),  // SINGLE_CHANGE
        (0.8 - weight_min) / (weight_max - weight_min),  // BLOCK_RELOCATE
        (0.5 - weight_min) / (weight_max - weight_min),  // CHAIN_MOVE
        (0.7 - weight_min) / (weight_max - weight_min),  // ROOM moves
        (0.6 - weight_min) / (weight_max - weight_min)   // MULTI_SWAP
    };
    std::vector<double> neighborhood_weights(num_neighborhoods);
    for (int i = 0; i < num_neighborhoods; ++i) {
        neighborhood_weights[i] = weight_min + (weight_max - weight_min) * neighborhood_scores[i];
    }
    
    int last_intensification_iteration = -intensification_interval;
    double last_best_score_value = best_solution.objective_value;
    bool just_improved_best_flag = false;
    int reheat_count = 0; // Track reheating to prevent oscillation
    int last_reheat_iteration = -stuck_threshold; // Track last reheat iteration
    int restart_count = 0; // Track restarts to prevent infinite loops
    
    for (int iteration = 0; iteration < max_iterations; ++iteration) {
        // Log progress mỗi 50 iterations để theo dõi
        if (iteration % 50 == 0 && iteration > 0) {
            double acceptance_rate = total_moves_attempted > 0 ? (100.0 * total_moves_accepted / total_moves_attempted) : 0.0;
            std::cout << "[PHASE3] [PROGRESS] Iteration " << iteration << "/" << max_iterations
                      << " | Temp: " << std::fixed << std::setprecision(6) << temperature
                      << " | Best: " << std::setprecision(2) << best_solution.objective_value
                      << " | Current: " << current_solution.objective_value
                      << " | Stuck: " << stuck_iterations_count
                      << " | No improve: " << iterations_without_improvement
                      << " | Acceptance: " << std::setprecision(1) << acceptance_rate << "%"
                      << " | Moves: " << total_moves_accepted << "/" << total_moves_attempted << std::endl;
        }
        
        if (temperature < MIN_TEMPERATURE) {
            const int REHEAT_BUFFER = 50;
            const int MAX_REHEATS = 1;
            
            if (iteration < max_iterations - REHEAT_BUFFER && 
                iterations_without_improvement > stuck_threshold &&
                reheat_count < MAX_REHEATS) {
                const double REHEAT_FACTOR = 0.3;
                temperature = initial_temperature * REHEAT_FACTOR;
                stuck_iterations_count = 0;
                reheat_count++;
                std::cout << "[PHASE3] [REHEAT] Iteration " << iteration 
                          << " | Temp too low, reheating to " << std::fixed << std::setprecision(6) << temperature
                          << " | Reheat count: " << reheat_count
                          << " | No improve: " << iterations_without_improvement << std::endl;
            } else {
                std::cout << "[PHASE3] [STOP] Iteration " << iteration 
                          << " | Temperature too low (" << std::fixed << std::setprecision(6) << temperature 
                          << ") and cannot reheat. Stopping." << std::endl;
                break;
            }
        }
        
        if (best_solution.objective_value > last_best_score_value) {
            just_improved_best_flag = true;
            double improvement = best_solution.objective_value - last_best_score_value;
            last_best_score_value = best_solution.objective_value;
            stuck_iterations_count = 0;
            std::cout << "[PHASE3] Iteration " << iteration 
                      << " | Best improved: " << std::fixed << std::setprecision(2) 
                      << best_solution.objective_value 
                      << " (+" << improvement << ")" << std::endl;
        } else {
            just_improved_best_flag = false;
        }
        
        bool intensification_mode = false;
        int intensification_interval_actual = intensification_interval;
        if (temperature < MIN_TEMPERATURE * 100) {
            intensification_interval_actual = 10;
        } else if (temperature < initial_temperature * 0.1) {
            intensification_interval_actual = 20;
        }
        
        if (just_improved_best_flag || 
            (iteration - last_intensification_iteration >= intensification_interval_actual && 
             temperature < initial_temperature * 0.3)) {
            intensification_mode = true;
            current_solution = best_solution;
            solution_index = build_index(current_solution, data);
            penalty_state = init_penalty_state(current_solution, data);
            last_intensification_iteration = iteration;
        }
        
        int moves_per_neighborhood = base_moves_per_neighborhood;
        if (temperature < MIN_TEMPERATURE * 100) {
            moves_per_neighborhood = base_moves_per_neighborhood * 3;
        } else if (temperature < initial_temperature * 0.1) {
            moves_per_neighborhood = (int)(base_moves_per_neighborhood * 2);
        }
        // When stuck, try more moves to escape local minimum
        if (stuck_iterations_count > stuck_threshold / 2) {
            moves_per_neighborhood = std::max(moves_per_neighborhood, base_moves_per_neighborhood * 3);
        }
        // Early termination: if stuck for too long with 0% acceptance, stop trying
        if (stuck_iterations_count > stuck_threshold && total_moves_attempted > 500 && total_moves_accepted == 0) {
            std::cout << "[PHASE3] [EARLY STOP] Stuck for " << stuck_iterations_count 
                      << " iterations with 0% acceptance rate. Stopping early." << std::endl;
            break;
        }
        if (just_improved_best_flag || intensification_mode) {
            moves_per_neighborhood = std::max(moves_per_neighborhood, base_moves_per_neighborhood * 2);
        }
        
        bool any_improved = false;
        std::vector<int> neighborhood_order(num_neighborhoods);
        std::iota(neighborhood_order.begin(), neighborhood_order.end(), 0);
        std::sort(neighborhood_order.begin(), neighborhood_order.end(),
                 [&](int a, int b) {
                     return neighborhood_weights[a] > neighborhood_weights[b];
                 });
        
        // OPTIMIZATION: Skip complex moves for very large problems
        const bool use_simple_moves_only = (problem_size > 200);
        
        for (int ordered_idx = 0; ordered_idx < num_neighborhoods; ++ordered_idx) {
            int neighborhood_idx = neighborhood_order[ordered_idx];
            
            // Skip complex neighborhoods for very large dense problems
            if (use_simple_moves_only) {
                // Only use SINGLE_CHANGE (0) and ROOM moves (3)
                // Skip BLOCK_RELOCATE (1), CHAIN_MOVE (2), MULTI_SWAP (4)
                if (neighborhood_idx == 1 || neighborhood_idx == 2 || neighborhood_idx == 4) {
                    continue;
                }
            }
            
            bool improved_in_neighborhood = false;
            
            for (int move_attempt = 0; move_attempt < moves_per_neighborhood; ++move_attempt) {
                MoveBuilder move_builder(current_solution, data);
                MoveSpec move_spec;
                bool move_generated = false;
                
                // Neighborhood selection with room moves
                if (neighborhood_idx == 0) {
                    move_generated = move_builder.build_single_change();
                } else if (neighborhood_idx == 1) {
                    move_generated = move_builder.build_block_relocate();
                } else if (neighborhood_idx == 2) {
                    move_generated = move_builder.build_chain_move(penalty_state);
                } else if (neighborhood_idx == 3) {
                    // Room-specific moves (50% swap, 50% shift)
                    std::uniform_int_distribution<int> room_move_dist(0, 1);
                    if (room_move_dist(rng) == 0) {
                        move_generated = move_builder.build_room_swap();
                    } else {
                        move_generated = move_builder.build_room_shift(penalty_state);
                    }
                } else {
                    move_generated = move_builder.build_multi_swap(2);
                }
                
                if (move_generated) {
                    move_spec = move_builder.get_spec();
                }
                if (!move_generated) {
                    infeasible_moves_count++;
                    continue;
                }
                
                MoveEvaluator move_evaluator(current_solution, solution_index, data, cache, lookups);
                MoveContext move_context = move_evaluator.evaluate(move_spec);
                if (!move_context.ok) {
                    infeasible_moves_count++;
                    continue;
                }
                
                MoveDelta move_delta;
                auto [candidate_score, objective_delta] = evaluate_move(
                    move_context, current_solution, solution_index, penalty_state, data, cache, lookups,
                    teacher_map, time_pref_map, initial_map, initial_solution, move_delta);
                
                total_moves_attempted++; // Count all evaluated moves
                
                if (candidate_score == 0.0 && objective_delta == 0.0 && move_context.move.type != Move::CHAIN_MOVE) {
                    infeasible_moves_count++;
                    continue;
                }
                
                if (move_delta.delta_hard > 0) {
                    infeasible_moves_count++;
                    continue;
                }
                
                AcceptMoveResult acceptance_result = accept_move(
                    move_context.move, current_solution, move_context.candidate,
                    candidate_score, objective_delta, temperature, tabu_list,
                    best_solution.objective_value,
                    tabu_rejected_count, sa_rejected_count);
                
                if (!acceptance_result.accepted) {
                    continue;
                }
                
                total_moves_accepted++;
                
                double move_reward = 0.0;
                if (candidate_score > best_solution.objective_value) {
                    move_reward = 1.0;
                } else if (candidate_score > current_solution.objective_value) {
                    move_reward = 0.7;
                }
                if (move_reward > 0.0) {
                    update_ema_weight(neighborhood_idx, move_reward, ema_beta,
                                    weight_min, weight_max,
                                    neighborhood_scores, neighborhood_weights);
                }
                
                commit_move(move_context, candidate_score,
                           current_solution, best_solution,
                           solution_index, iterations_without_improvement, data);
                
                update_penalty_state(move_context, penalty_state, move_delta, data);
                
                improved_in_neighborhood = true;
                any_improved = true;
                stuck_iterations_count = 0;
                break;
            }
            
            if (!improved_in_neighborhood) {
                update_ema_weight(neighborhood_idx, 0.0, ema_beta,
                                weight_min, weight_max,
                                neighborhood_scores, neighborhood_weights);
            }
        }
        
        if (!any_improved) {
            stuck_iterations_count++;
            // Log khi bị stuck
            if (stuck_iterations_count % 10 == 0 || stuck_iterations_count == stuck_threshold) {
                std::cout << "[PHASE3] [STUCK] Iteration " << iteration 
                          << " | Stuck count: " << stuck_iterations_count 
                          << " | No improve: " << iterations_without_improvement
                          << " | Temp: " << std::fixed << std::setprecision(6) << temperature
                          << " | Best: " << std::setprecision(2) << best_solution.objective_value
                          << " | Current: " << current_solution.objective_value
                          << " | Moves attempted: " << total_moves_attempted
                          << " | Moves accepted: " << total_moves_accepted
                          << " | Infeasible: " << infeasible_moves_count
                          << " | Tabu rejected: " << tabu_rejected_count
                          << " | SA rejected: " << sa_rejected_count << std::endl;
            }
        } else {
            stuck_iterations_count = 0;
        }
        
        const double NEIGHBORHOOD_DECAY_RATE = 0.9995;
        for (int i = 0; i < num_neighborhoods; ++i) {
            neighborhood_scores[i] *= NEIGHBORHOOD_DECAY_RATE;
            neighborhood_weights[i] = weight_min + (weight_max - weight_min) * neighborhood_scores[i];
        }
        
        const double STUCK_DECAY_FACTOR = 0.995;
        const int STUCK_DECAY_THRESHOLD = stuck_threshold / 2;
        const double MIN_TEMP_FOR_STUCK_DECAY = MIN_TEMPERATURE * 10;
        
        double effective_cooling_rate = cooling_rate;
        if (stuck_iterations_count > STUCK_DECAY_THRESHOLD && 
            temperature > MIN_TEMP_FOR_STUCK_DECAY) {
            effective_cooling_rate = STUCK_DECAY_FACTOR;
        }
        
        temperature *= effective_cooling_rate;
        
        const double REHEAT_TEMPERATURE_FACTOR = 0.3;
        const double REHEAT_TEMPERATURE_THRESHOLD = initial_temperature * 0.1;
        
        if (stuck_iterations_count > stuck_threshold && 
            temperature < REHEAT_TEMPERATURE_THRESHOLD &&
            (iteration - last_reheat_iteration) > stuck_threshold) {
            temperature = initial_temperature * REHEAT_TEMPERATURE_FACTOR;
            stuck_iterations_count = 0;
            last_reheat_iteration = iteration;
            std::cout << "[PHASE3] [REHEAT] Iteration " << iteration 
                      << " | Stuck " << stuck_iterations_count 
                      << " iterations, reheating to " << std::fixed << std::setprecision(6) << temperature << std::endl;
        }
        
        if (iterations_without_improvement > limit_no_improvement) {
            std::cout << "[PHASE3] [RESTART] Iteration " << iteration 
                      << " | No improvement for " << iterations_without_improvement 
                      << " iterations, restarting" << std::endl;
            current_solution = best_solution;
            iterations_without_improvement = 0;
            stuck_iterations_count = 0;
            
            const double RESTART_TEMP_THRESHOLD = initial_temperature * 0.2;
            const double RESTART_TEMP_FACTOR = 0.3;
            const int MAX_RESTARTS = 2;
            
            if (temperature < RESTART_TEMP_THRESHOLD && restart_count < MAX_RESTARTS) {
                temperature = initial_temperature * RESTART_TEMP_FACTOR;
                restart_count++;
                std::cout << "[PHASE3] [RESTART] Increasing temperature to " << std::fixed << std::setprecision(6) 
                          << temperature << " (restart #" << restart_count << ")" << std::endl;
            } else if (restart_count >= MAX_RESTARTS) {
                std::cout << "[PHASE3] [STOP] Max restarts (" << MAX_RESTARTS << ") reached. Stopping at iteration " 
                          << iteration << std::endl;
                break;
            }
            
            solution_index = build_index(current_solution, data);
            penalty_state = init_penalty_state(current_solution, data);
            
            std::shuffle(current_solution.assignments.begin(),
                        current_solution.assignments.end(), rng);
            
            const int MIN_LOCAL_SHAKES = 4;
            const int MAX_LOCAL_SHAKES = 40;
            const double TEMP_THRESHOLD_FOR_SHAKES = 0.1;
            int base_local_shakes = std::min(std::max(MIN_LOCAL_SHAKES,
                (int)current_solution.assignments.size() / 16), MAX_LOCAL_SHAKES);
            int num_local_shakes = (temperature > TEMP_THRESHOLD_FOR_SHAKES)
                ? base_local_shakes
                : std::max(1, (int)(base_local_shakes * temperature / TEMP_THRESHOLD_FOR_SHAKES));
            
            for (int shake_idx = 0; shake_idx < num_local_shakes; ++shake_idx) {
                MoveBuilder restart_builder(current_solution, data);
                MoveSpec restart_spec;
                bool restart_generated = restart_builder.build_single_change();
                if (!restart_generated) {
                    restart_generated = restart_builder.build_block_relocate();
                }
                if (restart_generated) {
                    restart_spec = restart_builder.get_spec();
                }
                if (!restart_generated) continue;
                
                MoveEvaluator restart_evaluator(current_solution, solution_index, data, cache, lookups);
                MoveContext restart_context = restart_evaluator.evaluate(restart_spec);
                if (!restart_context.ok) continue;
                
                bool all_have_classrooms = true;
                for (const auto &a : restart_context.candidate.assignments) {
                    if (a.classroom_id.empty()) {
                        all_have_classrooms = false;
                        break;
                    }
                }
                
                if (!all_have_classrooms || !check_hard_invariant(restart_context.candidate, data)) {
                    continue;
                }
                
                solution_index = restart_context.idx_after;
                current_solution = restart_context.candidate;
            }
            
        }
    }
}

} // namespace phase3

