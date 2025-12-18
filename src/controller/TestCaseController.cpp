#include "TestCaseController.h"
#include <drogon/drogon.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <chrono>
#if __cplusplus >= 201703L
#include <filesystem>
namespace fs = std::filesystem;
#else
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#endif

#include "../scheduler/phase1.h"
#include "../scheduler/phase2.h"
#include "../scheduler/phase3.h"

using json = nlohmann::json;
using namespace drogon;
using namespace std;
using namespace std::chrono;

void TestCaseController::runTestCase(const HttpRequestPtr &req,
                                     function<void(const HttpResponsePtr &)> &&callback)
{
    auto start_time = high_resolution_clock::now();
    
    try
    {
        auto body = req->getBody();
        if (body.empty())
        {
            LOG_WARN << "[TestCase] Empty body";
            json err;
            err["status"] = "error";
            err["message"] = "empty body";
            err["test_case_name"] = "";
            auto resp = HttpResponse::newHttpResponse();
            resp->setStatusCode(k400BadRequest);
            resp->setContentTypeCode(CT_APPLICATION_JSON);
            resp->setBody(err.dump());
            return callback(resp);
        }

        json request_json = json::parse(body);
        
        // Get test case name from request
        string test_case_name = request_json.value("test_case_name", "");
        if (test_case_name.empty())
        {
            json err;
            err["status"] = "error";
            err["message"] = "test_case_name is required";
            auto resp = HttpResponse::newHttpResponse();
            resp->setStatusCode(k400BadRequest);
            resp->setContentTypeCode(CT_APPLICATION_JSON);
            resp->setBody(err.dump());
            return callback(resp);
        }

        // Construct file path
        fs::path testcase_dir = "testcases";
        fs::path file_path = testcase_dir / test_case_name;
        
        // Check if file exists
        if (!fs::exists(file_path))
        {
            json err;
            err["status"] = "error";
            err["message"] = "Test case file not found: " + test_case_name;
            err["test_case_name"] = test_case_name;
            auto resp = HttpResponse::newHttpResponse();
            resp->setStatusCode(k404NotFound);
            resp->setContentTypeCode(CT_APPLICATION_JSON);
            resp->setBody(err.dump());
            return callback(resp);
        }

        // Read test case file
        ifstream file(file_path.string());
        if (!file.is_open())
        {
            json err;
            err["status"] = "error";
            err["message"] = "Cannot open test case file: " + test_case_name;
            err["test_case_name"] = test_case_name;
            auto resp = HttpResponse::newHttpResponse();
            resp->setStatusCode(k500InternalServerError);
            resp->setContentTypeCode(CT_APPLICATION_JSON);
            resp->setBody(err.dump());
            return callback(resp);
        }

        json test_data;
        file >> test_data;
        file.close();

        // Count statistics before processing
        int num_teachers = 0;
        int num_classrooms = 0;
        int num_courses = 0;
        int num_sections = 0;

        if (test_data.contains("teachers"))
        {
            num_teachers = test_data["teachers"].size();
        }

        if (test_data.contains("classrooms") && test_data["classrooms"].contains("classrooms"))
        {
            num_classrooms = test_data["classrooms"]["classrooms"].size();
        }

        if (test_data.contains("courses"))
        {
            num_courses = test_data["courses"].size();
            for (const auto &course : test_data["courses"])
            {
                if (course.contains("sections"))
                {
                    num_sections += course["sections"].size();
                }
            }
        }

        // Initialize problem data
        auto init_start = high_resolution_clock::now();
        ProblemData data = initialize_problem_from_json(test_data);
        auto init_end = high_resolution_clock::now();
        auto init_duration = duration_cast<milliseconds>(init_end - init_start).count();

        // Construct initial solution
        auto phase2_start = high_resolution_clock::now();
        InitialSolution init = construct_initial_solution(data);
        auto phase2_end = high_resolution_clock::now();
        auto phase2_duration = duration_cast<milliseconds>(phase2_end - phase2_start).count();

        // Find optimal solution
        auto phase3_start = high_resolution_clock::now();
        OptimalSolution opt = find_optimal_solution(data, init);
        auto phase3_end = high_resolution_clock::now();
        auto phase3_duration = duration_cast<milliseconds>(phase3_end - phase3_start).count();

        auto end_time = high_resolution_clock::now();
        auto total_duration = duration_cast<milliseconds>(end_time - start_time).count();

        // Build response JSON
        json jout;
        jout["status"] = "success";
        jout["test_case_name"] = test_case_name;
        
        // Statistics
        jout["statistics"] = json::object();
        jout["statistics"]["num_teachers"] = num_teachers;
        jout["statistics"]["num_classrooms"] = num_classrooms;
        jout["statistics"]["num_courses"] = num_courses;
        jout["statistics"]["num_sections"] = num_sections;
        jout["statistics"]["num_assignments"] = opt.assignments.size();

        // Timing information
        jout["timing"] = json::object();
        jout["timing"]["total_time_ms"] = total_duration;
        jout["timing"]["total_time_seconds"] = total_duration / 1000.0;
        jout["timing"]["phase1_init_ms"] = init_duration;
        jout["timing"]["phase2_initial_solution_ms"] = phase2_duration;
        jout["timing"]["phase3_optimization_ms"] = phase3_duration;

        // Solution results
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
    catch (const json::parse_error &ex)
    {
        auto end_time = high_resolution_clock::now();
        auto total_duration = duration_cast<milliseconds>(end_time - start_time).count();
        
        LOG_ERROR << "[TestCase] JSON parse error: " << ex.what();
        json err;
        err["status"] = "error";
        err["message"] = "JSON parse error: " + string(ex.what());
        err["timing"] = json::object();
        err["timing"]["total_time_ms"] = total_duration;
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k400BadRequest);
        resp->setContentTypeCode(CT_APPLICATION_JSON);
        resp->setBody(err.dump());
        callback(resp);
    }
    catch (const exception &ex)
    {
        auto end_time = high_resolution_clock::now();
        auto total_duration = duration_cast<milliseconds>(end_time - start_time).count();
        
        LOG_ERROR << "[TestCase] Exception: " << ex.what();
        json err;
        err["status"] = "error";
        err["message"] = ex.what();
        err["timing"] = json::object();
        err["timing"]["total_time_ms"] = total_duration;
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k500InternalServerError);
        resp->setContentTypeCode(CT_APPLICATION_JSON);
        resp->setBody(err.dump());
        callback(resp);
    }
}
