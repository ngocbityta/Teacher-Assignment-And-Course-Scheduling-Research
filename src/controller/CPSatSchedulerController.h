#pragma once

#include <drogon/HttpController.h>
#include "../scheduler/phase1.h"
#include "../scheduler/phase3.h"

// Forward declaration
struct ProblemData;
struct OptimalSolution;

// Function to solve with CP-SAT all constraints
OptimalSolution solve_with_cpsat_all_constraints(const ProblemData &data, int time_limit_seconds = 300);

class CPSatSchedulerController : public drogon::HttpController<CPSatSchedulerController> {
public:
    METHOD_LIST_BEGIN
    // POST /schedule-cpsat
    ADD_METHOD_TO(CPSatSchedulerController::schedule, "/schedule-cpsat", drogon::Post);
    METHOD_LIST_END

    void schedule(const drogon::HttpRequestPtr &req,
                  std::function<void (const drogon::HttpResponsePtr &)> &&callback);
};

