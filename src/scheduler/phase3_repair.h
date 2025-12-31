#pragma once

#include "phase3.h"
#include "phase3_invariants.h"

namespace phase3 {

OptimalSolution final_repair_solution(
    const OptimalSolution &solution,
    const ProblemData &data);

// Check all constraints (for debugging/validation)
bool check_all_constraints(
    const OptimalSolution &sol,
    const ProblemData &data);

} // namespace phase3

