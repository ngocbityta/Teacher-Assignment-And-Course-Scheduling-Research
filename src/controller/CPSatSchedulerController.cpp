#include "CPSatSchedulerController.h"
#include <drogon/drogon.h>
#include <nlohmann/json.hpp>

#include "../scheduler/phase1.h"
#include "../scheduler/SchedulerService.h"

using json = nlohmann::json;
using namespace drogon;
using namespace std;

void CPSatSchedulerController::schedule(const HttpRequestPtr &req,
                                        function<void(const HttpResponsePtr &)> &&callback)
{
    try
    {
        auto body = req->getBody();
        if (body.empty())
        {
            LOG_WARN << "[CPSatSchedule] Empty body";
            auto resp = HttpResponse::newHttpResponse();
            resp->setStatusCode(k400BadRequest);
            resp->setContentTypeCode(CT_APPLICATION_JSON);
            resp->setBody(R"({"status":"error","message":"empty body"})");
            return callback(resp);
        }

        json jin = json::parse(body);

        ProblemData data = initialize_problem_from_json(jin);

        int time_limit = 30;
        if (jin.contains("time_limit_seconds") && jin["time_limit_seconds"].is_number())
        {
            time_limit = jin["time_limit_seconds"];
        }

        OptimalSolution opt = SchedulerService::solve_exact(data, time_limit);

        if (opt.assignments.empty()) {
            json err;
            err["status"] = "error";
            err["message"] = "CP-SAT failed: No feasible solution found or room assignment failed";
            auto resp = HttpResponse::newHttpResponse();
            resp->setStatusCode(k400BadRequest);
            resp->setContentTypeCode(CT_APPLICATION_JSON);
            resp->setBody(err.dump());
            return callback(resp);
        }

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
        LOG_ERROR << "[CPSatSchedule] Exception: " << ex.what();
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
