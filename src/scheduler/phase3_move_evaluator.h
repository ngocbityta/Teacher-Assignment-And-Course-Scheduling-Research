#pragma once

#include "phase3.h"
#include "phase3_move.h"
#include "phase3_index.h"
#include "phase3_invariants.h"
#include "phase3_penalty_state.h"
#include <string>

namespace phase3 {

// Move context for evaluation
struct MoveContext {
    bool ok;
    Move move;
    OptimalSolution candidate;
    SolIndex idx_after;
    std::vector<AssignmentChange> changes;
};

// Result of trying an assignment change
struct TryAssignmentResult {
    bool ok;
    std::string classroom_id;  // HARD CONSTRAINT: Must be non-empty
    IndexDelta delta;
};

// Move delta for evaluation
struct MoveDelta {
    int delta_hard = 0;  // Must be 0 for move to be accepted
    double delta_soft_local = 0.0;
    double delta_workload = 0.0;
    double delta_compactness = 0.0;
};

// Computes delta_hard and delta_soft for a move
class MoveEvaluator {
private:
    const OptimalSolution &current;
    const SolIndex &current_idx;
    const ProblemData &data;
    const CachedIndices &cache;
    const CachedLookups &lookups;
    
    // Check if assignment is feasible (returns classroom_id or empty string)
    static std::string is_feasible(const OptimalSolution::Assignment &a,
                                   const SolIndex &idx, 
                                   const ProblemData &data,
                                   const CachedIndices &cache,
                                   const CachedLookups &lookups);
    
    // Check if assignment is feasible when removing old assignment
    static std::string is_feasible_with_removal(
        const OptimalSolution::Assignment &old_a,
        const OptimalSolution::Assignment &new_a,
        const SolIndex &idx,
        const ProblemData &data,
        const CachedIndices &cache,
        const CachedLookups &lookups);
    
    // Try assignment change and return result
    static TryAssignmentResult try_assignment_change(
        const OptimalSolution::Assignment &old_assignment,
        OptimalSolution::Assignment new_assignment,
        const SolIndex &solution_index,
        const ProblemData &data,
        const CachedIndices &cache,
        const CachedLookups &lookups);
    
    // Expand move into assignment changes
    static std::vector<AssignmentChange> expand_move(
        const Move &move,
        const OptimalSolution &current_solution,
        const OptimalSolution &candidate_solution);
    
public:
    MoveEvaluator(const OptimalSolution &sol, const SolIndex &idx, const ProblemData &d,
                 const CachedIndices &c, const CachedLookups &l);
    
    // Evaluate move and return context
    // Returns MoveContext with ok=false if move violates hard constraints
    MoveContext evaluate(const MoveSpec &spec);
};

// Calculate delta_hard (number of hard constraint violations)
// Returns 0 if no violations, >0 if violations exist
int calculate_delta_hard(const MoveContext &ctx, const ProblemData &data, 
                         const CachedIndices &cache, const CachedLookups &lookups,
                         const SolIndex &current_idx);

// Evaluate move and compute deltas
// Returns (candidate_score, objective_delta) and populates move_delta
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
    MoveDelta &delta_out);

} // namespace phase3

