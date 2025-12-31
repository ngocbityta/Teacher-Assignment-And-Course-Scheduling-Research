#pragma once
#include "phase1.h"
#include <vector>

enum class Phase2Mode {
    FEASIBLE_ONLY,
    LIGHT_QUALITY
};

struct InitialSolution {
    struct Assignment {
        std::string teacher_id;
        std::string course_id;
        std::string section_id;
        std::string day;
        std::string period;
        std::string classroom_id; // lớp học được phân bổ
        
        // Metadata phục vụ Phase 3
        std::string initial_teacher;
        std::string initial_timeslot;
    };

    std::vector<Assignment> assignments;
};

InitialSolution construct_initial_solution(
    const ProblemData &data,
    int random_seed = 0,
    bool shuffle_sections = false,
    bool shuffle_teachers = false,
    bool shuffle_timeslots = false);

std::vector<InitialSolution> generate_solution_pool(
    const ProblemData &data,
    int K = 8,
    double min_diversity = -1.0,
    int max_solutions = 10);

std::vector<InitialSolution> select_diverse_solutions(
    const std::vector<InitialSolution> &pool,
    int N);


double evaluate_phase2_solution_quick(
    const std::vector<InitialSolution> &pool,
    int k,
    const ProblemData &data);

bool is_phase2_solution_good_enough(
    const InitialSolution &sol,
    const ProblemData &data,
    double min_score = 0.3);