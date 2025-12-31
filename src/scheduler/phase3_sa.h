#pragma once

#include "phase3.h"
#include "phase3_move.h"
#include "phase3_move_evaluator.h"
#include "phase3_move_committer.h"
#include "phase3_index.h"
#include "phase3_penalty_state.h"
#include "phase3_invariants.h"
#include <deque>
#include <unordered_set>

namespace phase3 {

// Tabu list for SA
struct SimpleTabu {
    std::deque<std::string> tabu_queue_teacher;
    std::deque<std::string> tabu_queue_timeslot;
    std::deque<std::string> tabu_queue_chain_teacher_day;
    std::unordered_set<std::string> tabu_set_teacher;
    std::unordered_set<std::string> tabu_set_timeslot;
    std::unordered_set<std::string> tabu_set_chain_teacher_day;
    size_t tenure;
    
    SimpleTabu(size_t tenure_val = 50);
    bool is_tabu(const std::string &sig_teacher, const std::string &sig_timeslot, const std::string &sig_chain_teacher_day = "") const;
    void add_tabu(const std::string &sig_teacher, const std::string &sig_timeslot, const std::string &sig_chain_teacher_day = "");
};

// Accept move result
struct AcceptMoveResult {
    bool accepted;
    bool rejected_by_tabu;
    bool rejected_by_sa;
    std::string sig_teacher;
    std::string sig_timeslot;
    std::string sig_chain_teacher_day;
};

// SA acceptance criterion
bool should_accept_move(
    double objective_delta,
    double temperature,
    int &sa_rejected,
    double best_score = 0.0,
    double candidate_score = 0.0,
    bool is_at_best = false);

// Accept move (tabu + SA)
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
    int &sa_rejected_count);

// Extract tabu keys from move
std::tuple<std::string, std::string, std::string> extract_tabu_keys(
    const OptimalSolution &old_sol,
    const OptimalSolution &new_sol,
    const Move &move);

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
    const std::unordered_map<std::string, OptimalSolution::Assignment> &initial_map);

} // namespace phase3

