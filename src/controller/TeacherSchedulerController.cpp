#include "TeacherSchedulerController.h"
#include <drogon/drogon.h>
#include <nlohmann/json.hpp>

#include "../scheduler/phase1.h"
#include "../scheduler/phase2.h"
#include "../scheduler/phase3.h"

using json = nlohmann::json;
using namespace drogon;
using namespace std;

void TeacherSchedulerController::schedule(const HttpRequestPtr &req,
                                          function<void(const HttpResponsePtr &)> &&callback)
{
    try
    {
        auto body = req->getBody();
        if (body.empty())
        {
            LOG_WARN << "[Schedule] Empty body";
            auto resp = HttpResponse::newHttpResponse();
            resp->setStatusCode(k400BadRequest);
            resp->setContentTypeCode(CT_APPLICATION_JSON);
            resp->setBody(R"({"status":"error","message":"empty body"})");
            return callback(resp);
        }

        json jin = json::parse(body);

        ProblemData data = initialize_problem_from_json(jin);

        InitialSolution init = construct_initial_solution(data);
        
        int phase2_objective = evaluate_initial_solution(data, init);

        OptimalSolution opt = find_optimal_solution(data, init);
        
        cout << "\n========== Phase 3 Result Summary ==========" << endl;
        cout << "  Objective value: " << opt.objective_value << endl;
        cout << "  Improvement: " << (opt.objective_value - phase2_objective) << " (" 
             << ((opt.objective_value - phase2_objective) * 100.0 / max(abs(phase2_objective), 1)) << "%)" << endl;
        cout << "==============================================" << endl;

        // Build response JSON
        json jout;
        jout["status"] = "success";
        jout["solution"] = json::object();
        jout["solution"]["objective_value"] = opt.objective_value;
        jout["solution"]["assignments"] = json::array();

        for (const auto &a : opt.assignments)
        {
            json ja;
            ja["teacher_id"] = a.teacher_id;
            ja["course_id"] = a.course_id;
            ja["section_id"] = a.section_id;
            ja["day"] = a.day;
            ja["period"] = a.period;
            ja["classroom_id"] = a.classroom_id;
            jout["solution"]["assignments"].push_back(ja);
        }

        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k200OK);
        resp->setContentTypeCode(CT_APPLICATION_JSON);
        resp->setBody(jout.dump());
        callback(resp);
    }
    catch (const exception &ex)
    {
        LOG_ERROR << "[Schedule] Exception: " << ex.what();
        json err;
        err["status"] = "error";
        err["message"] = ex.what();
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k500InternalServerError);
        resp->setContentTypeCode(CT_APPLICATION_JSON);
        resp->setBody(err.dump());
        callback(resp);
    }
}

void TeacherSchedulerController::health(const HttpRequestPtr &req,
                                        function<void(const HttpResponsePtr &)> &&callback)
{
    json out;
    out["status"] = "ok";
    out["message"] = "alive";

    auto resp = HttpResponse::newHttpResponse();
    resp->setStatusCode(k200OK);
    resp->setContentTypeCode(CT_APPLICATION_JSON);
    resp->setBody(out.dump());
    callback(resp);
}
