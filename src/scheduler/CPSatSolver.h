#pragma once

#include "phase1.h"
#include "phase3.h"
#include "ortools/sat/cp_model.h"
#include "ortools/sat/cp_model_solver.h"
#include <map>
#include <tuple>
#include <vector>

using namespace operations_research;
using namespace operations_research::sat;

class CPSatSolver {
public:
    CPSatSolver(const ProblemData& data, int time_limit_seconds = 30);
    
    OptimalSolution solve();

private:
    const ProblemData& data_;
    int time_limit_seconds_;
    
    CpModelBuilder model_;
    
    // Decision variables
    // Y[i][j][k][l][m0] = 1 if teacher i teaches section k of course j at day l, start period m0
    using Key = std::tuple<int,int,int,int,int>;
    std::map<Key, BoolVar> Y_;
    
    // Y_room[i][j][k][l][m0][room_idx]
    using RoomKey = std::tuple<int,int,int,int,int,int>;
    std::map<RoomKey, BoolVar> Y_room_;
    
    // Helpers
    void create_variables();
    void add_hard_constraints();
    void add_soft_constraints();
    OptimalSolution extract_solution(const CpSolverResponse& response);
    
    // Constraint helpers
    void add_one_section_assignment_constraint();
    void add_room_coupling_constraint();
    void add_teacher_time_overlap_constraint();
    void add_room_capacity_overlap_constraint();
    void add_teacher_course_constraints();
    void add_course_section_overlap_constraint();
    
    // Data helpers
    int get_required_periods(const std::string &course_id, const std::string &section_id) const;
    int find_period_index(const std::string &period) const;
};
