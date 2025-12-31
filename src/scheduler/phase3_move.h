#pragma once

#include "phase3.h"
#include <vector>
#include <string>

namespace phase3 {

// Move data structures only (no logic)
// These are pure data containers for representing moves

struct Block {
    int assignment_idx;
    std::string period;
    int required_periods;
};

struct Move {
    enum Type {
        SINGLE_CHANGE,
        BLOCK_RELOCATE,
        CHAIN_MOVE,
        MULTI_SWAP,
        ROOM_SWAP,  // Swap rooms between two assignments
        ROOM_SHIFT  // Shift room for one assignment to a better room
    };
    Type type;
    std::vector<int> indices;
    std::vector<Block> chain;
};

struct MoveSpec {
    Move::Type type = Move::SINGLE_CHANGE;
    std::vector<int> assignment_indices;
    std::string new_teacher_id;
    std::string new_day;
    std::string new_period;
    std::vector<Block> chain;
    std::vector<std::string> new_days; // For MULTI_SWAP: new days for each assignment
    std::vector<std::string> new_room_ids; // For ROOM_SWAP/ROOM_SHIFT: new room IDs
};

struct AssignmentChange {
    int idx;
    OptimalSolution::Assignment old_a;
    OptimalSolution::Assignment new_a;
};

} // namespace phase3

