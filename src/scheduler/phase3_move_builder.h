#pragma once

#include "phase3.h"
#include "phase3_move.h"
#include "phase3_penalty_state.h"
#include <random>
#include <chrono>

namespace phase3 {

// Generates candidate moves that NEVER violate hard constraints
// Especially ensures eligibility and classroom existence
class MoveBuilder {
private:
    MoveSpec spec;
    const OptimalSolution &sol;
    const ProblemData &data;
    static std::mt19937 rng;
    
    static int select_random_assignment(const OptimalSolution &sol);
    static std::pair<std::string, std::string> select_random_timeslot(const ProblemData &data);
    static std::pair<std::string, std::string> select_weighted_timeslot(
        const ProblemData &data, const PenaltyState &penalty_state, const std::string &teacher_id);
    static std::string select_random_teacher(const std::vector<std::string> &teachers);
    static std::vector<Block> extract_chain(const std::string &teacher_id, const std::string &day, 
                                           const OptimalSolution &sol, const ProblemData &data);
    static void rotate_chain(std::vector<Block> &chain, const ProblemData &data);
    
public:
    MoveBuilder(const OptimalSolution &sol_ref, const ProblemData &data_ref);
    
    // Build single change move (teacher or timeslot change)
    // NEVER generates moves that violate hard constraints
    bool build_single_change();
    
    // Build block relocate move (timeslot change)
    // NEVER generates moves that violate hard constraints
    bool build_block_relocate();
    
    // Build chain move
    // NEVER generates moves that violate hard constraints
    bool build_chain_move(const PenaltyState &penalty_state);
    
    // Build multi-swap move
    bool build_multi_swap(int num_swaps = 2);
    
    // Build room swap move (swap rooms between two assignments)
    bool build_room_swap();
    
    // Build room shift move (change room for one assignment to a better room)
    bool build_room_shift(const PenaltyState &penalty_state);
    
    MoveSpec get_spec() const { return spec; }
    
    bool build(Move::Type move_type, const PenaltyState &state);
};

} // namespace phase3

