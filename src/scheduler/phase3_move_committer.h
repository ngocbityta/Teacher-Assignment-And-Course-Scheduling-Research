#pragma once

#include "phase3.h"
#include "phase3_move.h"
#include "phase3_move_evaluator.h"
#include "phase3_index.h"
#include "phase3_penalty_state.h"
#include "phase3_invariants.h"
#include <cassert>

namespace phase3 {

void commit_move(
    const MoveContext &move_context,
    double candidate_score,
    OptimalSolution &current_solution,
    OptimalSolution &best_solution,
    SolIndex &current_solution_index,
    int &iterations_without_improvement,
    const ProblemData &data);

// Update penalty state after move
void update_penalty_state(
    const MoveContext &move_context,
    PenaltyState &penalty_state,
    const MoveDelta &move_delta,
    const ProblemData &data);

} // namespace phase3

