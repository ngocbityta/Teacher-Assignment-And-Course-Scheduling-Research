#include "SchedulerService.h"
#include "CPSatSolver.h"
#include <iostream>

using namespace std;

OptimalSolution SchedulerService::solve_heuristic(const ProblemData& data, 
                                                int max_solutions,
                                                int time_limit_seconds)
{
    // Phase 2: Construct initial solution
    InitialSolution init_sol = construct_initial_solution(data);
    
    // Check if Phase 2 failed to find a valid initial solution
    if (init_sol.assignments.empty()) {
        cout << "[SchedulerService] Phase 2 failed to find initial solution." << endl;
        return OptimalSolution();
    }

    // Phase 3: Optimize using Simulated Annealing
    // Phase 3 logic handles constraint repair and optimization
    OptimalSolution opt_sol = find_optimal_solution(data, init_sol);
    
    return opt_sol;
}

OptimalSolution SchedulerService::solve_exact(const ProblemData& data, 
                                            int time_limit_seconds)
{
    CPSatSolver solver(data, time_limit_seconds);
    return solver.solve();
}
