#pragma once
#include "phase1.h"
#include <vector>
using namespace std;

struct InitialSolution {
    struct Assignment {
        string teacher_id;
        string course_id;
        string section_id;
        string day;
        string period;
        string classroom_id; // lớp học được phân bổ
        
        // Metadata phục vụ Phase 3
        string initial_teacher;
        string initial_timeslot;
    };

    std::vector<Assignment> assignments;
};

InitialSolution construct_initial_solution(
    const ProblemData &data,
    int random_seed = 0,
    bool shuffle_sections = false,
    bool shuffle_teachers = false,
    bool shuffle_timeslots = false);

vector<InitialSolution> generate_solution_pool(
    const ProblemData &data,
    int K = 8,
    double min_diversity = -1.0,
    int max_solutions = 10);

vector<InitialSolution> select_diverse_solutions(
    const vector<InitialSolution> &pool,
    int N);

InitialSolution assign_rooms_phase2(const ProblemData &data, const InitialSolution &initial);

double evaluate_phase2_solution_quick(
    const InitialSolution &sol,
    const ProblemData &data);

bool is_phase2_solution_good_enough(
    const InitialSolution &sol,
    const ProblemData &data,
    double min_score = 0.3);