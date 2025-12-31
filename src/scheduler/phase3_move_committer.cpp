#include "phase3_move_committer.h"
#include "phase3_invariants.h"
#include "phase_common.h"
#include <cassert>

namespace phase3 {

void commit_move(
    const MoveContext &move_context,
    double candidate_score,
    OptimalSolution &current_solution,
    OptimalSolution &best_solution,
    SolIndex &current_solution_index,
    int &iterations_without_improvement,
    const ProblemData &data) {
    
    current_solution_index.num_periods = move_context.idx_after.num_periods;
    current_solution_index.teacher_busy_idx = move_context.idx_after.teacher_busy_idx;
    current_solution_index.course_slot_section_idx = move_context.idx_after.course_slot_section_idx;
    current_solution_index.course_teachers = move_context.idx_after.course_teachers;
    current_solution_index.course_teacher_sections = move_context.idx_after.course_teacher_sections;
    current_solution_index.classroom_busy_idx = move_context.idx_after.classroom_busy_idx;
    
    current_solution = move_context.candidate;
    current_solution.objective_value = candidate_score;
    
    // Only update best solution if candidate has better score AND passes hard constraints
    if (candidate_score > best_solution.objective_value) {
        // Verify the candidate passes all hard constraints before becoming best
        if (check_hard_invariant(move_context.candidate, data)) {
            best_solution = move_context.candidate;
            best_solution.objective_value = candidate_score;
            iterations_without_improvement = 0;
        } else {
            iterations_without_improvement++;
        }
    } else {
        iterations_without_improvement++;
    }
}

void update_penalty_state(
    const MoveContext &move_context,
    PenaltyState &penalty_state,
    const MoveDelta &move_delta,
    const ProblemData &data) {
    
    // Use delta updates for all moves - apply_change already handles incremental updates
    // For ROOM_SWAP/ROOM_SHIFT, apply_change will only update affected room slots (O(#slots affected))
    // For chain/block moves, apply_change will only update affected teacher slots and compactness
    for (const auto &assignment_change : move_context.changes) {
        penalty_state.apply_change(assignment_change, data);
    }
}

} // namespace phase3

