#pragma once

#include "phase1.h"
#include "phase2.h"
#include "phase3.h"

class SchedulerService {
public:
    static OptimalSolution solve_heuristic(const ProblemData& data, 
                                          int max_solutions = 10,
                                          int time_limit_seconds = 30);
                                          
    static OptimalSolution solve_exact(const ProblemData& data, 
                                      int time_limit_seconds = 30);
};
