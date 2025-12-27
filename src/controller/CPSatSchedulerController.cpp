#include "CPSatSchedulerController.h"
#include <drogon/drogon.h>
#include <nlohmann/json.hpp>

#include "../scheduler/phase1.h"
#include "../scheduler/phase3.h"
#include "ortools/sat/cp_model.h"
#include "ortools/sat/cp_model_solver.h"
#include <numeric>
#include <cmath>
#include <algorithm>

using json = nlohmann::json;
using namespace drogon;
using namespace std;
using namespace operations_research;
using namespace operations_research::sat;

static int get_required_periods(const ProblemData &data, const string &course_id, const string &section_id)
{
    for (const auto &c : data.courses)
        if (c.id == course_id)
            for (const auto &s : c.sections)
                if (s.id == section_id)
                    return s.required_periods;
    return 1;
}

static int find_period_index(const vector<string> &periods, const string &period)
{
    for (int m = 0; m < (int)periods.size(); ++m)
    {
        if (periods[m] == period)
            return m;
    }
    return -1;
}

static void extract_solution_to_optimal(
    const CpSolverResponse &response,
    const map<tuple<int,int,int,int,int>, BoolVar> &Y,
    const map<tuple<int,int,int,int,int,int>, BoolVar> &Y_room,
    const ProblemData &data,
    int R,
    OptimalSolution &sol)
{
    for (auto &kv : Y)
    {
        if (SolutionBooleanValue(response, kv.second))
        {
            int i,j,k,l,m0;
            tie(i,j,k,l,m0) = kv.first;
            OptimalSolution::Assignment a;
            a.teacher_id = data.teachers[i].id;
            a.course_id = data.courses[j].id;
            a.section_id = data.courses[j].sections[k].id;
            a.day = data.classrooms.days[l];
            a.period = data.classrooms.periods[m0];
            a.classroom_id = "";
            
            if (R > 0)
                for (int room_idx = 0; room_idx < R; ++room_idx)
                {
                    auto it_room = Y_room.find({i,j,k,l,m0,room_idx});
                    if (it_room != Y_room.end() && SolutionBooleanValue(response, it_room->second))
                    {
                        a.classroom_id = data.classrooms.classrooms[room_idx].id;
                        break;
                    }
                }
            sol.assignments.push_back(a);
        }
    }
}

OptimalSolution solve_with_cpsat_all_constraints(const ProblemData &data, int time_limit_seconds)
{
    CpModelBuilder model;
    int I = (int)data.teachers.size();
    int J = (int)data.courses.size();
    int L = (int)data.classrooms.days.size();
    int M = (int)data.classrooms.periods.size();
    vector<int> S(J);
    for (int j = 0; j < J; ++j)
        S[j] = (int)data.courses[j].sections.size();

    vector<vector<bool>> eligible(I, vector<bool>(J, false));
    for (int i = 0; i < I; ++i)
        for (int j = 0; j < J; ++j)
            eligible[i][j] = find(data.teachers[i].eligible_courses.begin(),
                                  data.teachers[i].eligible_courses.end(),
                                  data.courses[j].id) != data.teachers[i].eligible_courses.end();

    using Key = tuple<int,int,int,int,int>;
    map<Key, BoolVar> Y;
    int R = (int)data.classrooms.classrooms.size();
    using RoomKey = tuple<int,int,int,int,int,int>;
    map<RoomKey, BoolVar> Y_room;

    // Create decision variables
    for (int i = 0; i < I; ++i)
        for (int j = 0; j < J; ++j)
            if (eligible[i][j])
                for (int k = 0; k < S[j]; ++k)
                {
                    int r = data.courses[j].sections[k].required_periods;
                    int seats = data.courses[j].sections[k].required_seats;
                    for (int l = 0; l < L; ++l)
                        for (int m0 = 0; m0 + r <= M; ++m0)
                        {
                            Y[{i,j,k,l,m0}] = model.NewBoolVar();
                            if (R > 0)
                                for (int room_idx = 0; room_idx < R; ++room_idx)
                                    if (data.classrooms.classrooms[room_idx].capacity >= seats)
                                        Y_room[{i,j,k,l,m0,room_idx}] = model.NewBoolVar();
                        }
                }

    // Hard constraint: Each section must be assigned exactly once
    for (int j = 0; j < J; ++j)
        for (int k = 0; k < S[j]; ++k)
        {
            LinearExpr sum = 0;
            for (int i = 0; i < I; ++i)
                if (eligible[i][j])
                    for (int l = 0; l < L; ++l)
                        for (int m0 = 0; m0 < M; ++m0)
                        {
                            auto it = Y.find({i,j,k,l,m0});
                            if (it != Y.end())
                                sum += it->second;
                        }
            model.AddEquality(sum, 1);
        }
    
    // Hard constraint: Room assignment must match assignment
    if (R > 0)
    {
        for (int i = 0; i < I; ++i)
            for (int j = 0; j < J; ++j)
                if (eligible[i][j])
                    for (int k = 0; k < S[j]; ++k)
                    {
                        int r = data.courses[j].sections[k].required_periods;
                        for (int l = 0; l < L; ++l)
                            for (int m0 = 0; m0 + r <= M; ++m0)
                            {
                                auto it_y = Y.find({i,j,k,l,m0});
                                if (it_y == Y.end()) continue;
                                LinearExpr room_sum = 0;
                                for (int room_idx = 0; room_idx < R; ++room_idx)
                                {
                                    auto it_room = Y_room.find({i,j,k,l,m0,room_idx});
                                    if (it_room != Y_room.end())
                                        room_sum += it_room->second;
                                }
                                model.AddEquality(room_sum, it_y->second);
                            }
                    }
    }

    // Hard constraint: Teacher cannot teach two things at same time
    for (int i = 0; i < I; ++i)
        for (int l = 0; l < L; ++l)
            for (int m = 0; m < M; ++m)
            {
                LinearExpr load = 0;
                for (int j = 0; j < J; ++j)
                    if (eligible[i][j])
                        for (int k = 0; k < S[j]; ++k)
                        {
                            int r = data.courses[j].sections[k].required_periods;
                            for (int m0 = max(0, m - r + 1); m0 <= m; ++m0)
                            {
                                auto it = Y.find({i,j,k,l,m0});
                                if (it != Y.end())
                                    load += it->second;
                            }
                        }
                model.AddLessOrEqual(load, 1);
            }

    // Hard constraint: Room capacity
    if (R > 0)
    {
        for (int j = 0; j < J; ++j)
            for (int k = 0; k < S[j]; ++k)
            {
                int seats = data.courses[j].sections[k].required_seats;
                for (int i = 0; i < I; ++i)
                    if (eligible[i][j])
                    {
                        int r = data.courses[j].sections[k].required_periods;
                        for (int l = 0; l < L; ++l)
                            for (int m0 = 0; m0 + r <= M; ++m0)
                                for (int room_idx = 0; room_idx < R; ++room_idx)
                                    if (data.classrooms.classrooms[room_idx].capacity < seats)
                                    {
                                        auto it_room = Y_room.find({i,j,k,l,m0,room_idx});
                                        if (it_room != Y_room.end())
                                            model.AddEquality(it_room->second, 0);
                                    }
                    }
            }
        
        // Hard constraint: Room cannot be double-booked
        for (int room_idx = 0; room_idx < R; ++room_idx)
            for (int l = 0; l < L; ++l)
                for (int m = 0; m < M; ++m)
                {
                    LinearExpr room_load = 0;
                    for (int i = 0; i < I; ++i)
                        for (int j = 0; j < J; ++j)
                            if (eligible[i][j])
                                for (int k = 0; k < S[j]; ++k)
                                {
                                    int r = data.courses[j].sections[k].required_periods;
                                    for (int m0 = max(0, m - r + 1); m0 <= m; ++m0)
                                    {
                                        if (m0 + r > M) continue;
                                        auto it_room = Y_room.find({i,j,k,l,m0,room_idx});
                                        if (it_room != Y_room.end())
                                            room_load += it_room->second;
                                    }
                                }
                    model.AddLessOrEqual(room_load, 1);
                }
    }
    else
    {
        // No rooms: check if any section needs more seats than max capacity
        int max_cap = 0;
        for (auto &c : data.classrooms.classrooms)
            max_cap = max(max_cap, c.capacity);
        for (int j = 0; j < J; ++j)
            for (int k = 0; k < S[j]; ++k)
                if (data.courses[j].sections[k].required_seats > max_cap && max_cap > 0)
                    for (int i = 0; i < I; ++i)
                        for (int l = 0; l < L; ++l)
                            for (int m0 = 0; m0 < M; ++m0)
                            {
                                auto it = Y.find({i,j,k,l,m0});
                                if (it != Y.end())
                                    model.AddEquality(it->second, 0);
                            }
    }

    // Hard constraint: Course min/max teachers
    for (int j = 0; j < J; ++j)
    {
        int min_t = data.courses[j].min_teachers;
        int max_t = data.courses[j].max_teachers;
        int eligible_count = 0;
        for (int i = 0; i < I; ++i)
            if (eligible[i][j]) eligible_count++;
        
        if (min_t > 0 || (max_t > 0 && max_t < eligible_count))
        {
            map<int, BoolVar> teacher_indicator;
            for (int i = 0; i < I; ++i)
            {
                if (!eligible[i][j]) continue;
                BoolVar indicator = model.NewBoolVar();
                teacher_indicator[i] = indicator;
                LinearExpr sum = 0;
                for (int k = 0; k < S[j]; ++k)
                {
                    int r = data.courses[j].sections[k].required_periods;
                    for (int l = 0; l < L; ++l)
                        for (int m0 = 0; m0 + r <= M; ++m0)
                        {
                            auto it = Y.find({i,j,k,l,m0});
                            if (it != Y.end())
                                sum += it->second;
                        }
                }
                model.AddGreaterOrEqual(sum, indicator);
                model.AddLessOrEqual(sum, S[j] * L * M * indicator);
            }
            LinearExpr total = 0;
            for (auto &kv : teacher_indicator)
                total += kv.second;
            if (min_t > 0) model.AddGreaterOrEqual(total, min_t);
            if (max_t > 0 && max_t < eligible_count) model.AddLessOrEqual(total, max_t);
        }
    }
    
    // Hard constraint: Sections of same course cannot overlap
    for (int j = 0; j < J; ++j)
    {
        if (S[j] <= 1) continue;
        for (int l = 0; l < L; ++l)
            for (int m = 0; m < M; ++m)
            {
                LinearExpr sections = 0;
                for (int k = 0; k < S[j]; ++k)
                {
                    int r = data.courses[j].sections[k].required_periods;
                    for (int m0 = max(0, m - r + 1); m0 <= m; ++m0)
                    {
                        if (m0 + r > M) continue;
                        for (int i = 0; i < I; ++i)
                        {
                            if (!eligible[i][j]) continue;
                            auto it = Y.find({i,j,k,l,m0});
                            if (it != Y.end())
                                sections += it->second;
                        }
                    }
                }
                model.AddLessOrEqual(sections, 1);
            }
    }

    // ========== SOFT CONSTRAINTS - Objective Function ==========
    const double w_course_pref = 1.0;
    const double w_time_pref = 1.0;
    const double w_workload_balance = 5.0;
    const double w_compactness = 3.0;

    LinearExpr objective = 0;

    // 1. Course preference (maximize)
    for (int i = 0; i < I; ++i)
        for (int j = 0; j < J; ++j)
            if (eligible[i][j])
            {
                int course_pref_score = 0;
                auto pc_it = data.teachers[i].course_pref.find(data.courses[j].id);
                if (pc_it != data.teachers[i].course_pref.end())
                    course_pref_score = pc_it->second;

                for (int k = 0; k < S[j]; ++k)
                {
                    int r = data.courses[j].sections[k].required_periods;
                    for (int l = 0; l < L; ++l)
                        for (int m0 = 0; m0 + r <= M; ++m0)
                        {
                            auto it = Y.find({i,j,k,l,m0});
                            if (it != Y.end())
                                objective += w_course_pref * course_pref_score * it->second;
                        }
                }
            }

    // 2. Time preference (maximize)
    for (int i = 0; i < I; ++i)
        for (int j = 0; j < J; ++j)
            if (eligible[i][j])
                for (int k = 0; k < S[j]; ++k)
                {
                    int r = data.courses[j].sections[k].required_periods;
                    for (int l = 0; l < L; ++l)
                        for (int m0 = 0; m0 + r <= M; ++m0)
                        {
                            auto it = Y.find({i,j,k,l,m0});
                            if (it == Y.end()) continue;

                            int time_pref_score = 0;
                            string day = data.classrooms.days[l];
                            for (int t = 0; t < r && m0 + t < M; ++t)
                            {
                                string period = data.classrooms.periods[m0 + t];
                                string key = day + "|" + period;
                                
                                for (const auto &tp : data.teachers[i].time_pref)
                                {
                                    if (tp.day == day && tp.period == period)
                                    {
                                        time_pref_score += tp.score;
                                        break;
                                    }
                                }
                            }
                            objective += w_time_pref * time_pref_score * it->second;
                        }
                }

    model.Maximize(objective);

    // Solve
    Model sat_model;
    ostringstream param_stream;
    param_stream << "max_time_in_seconds:" << time_limit_seconds << " "
                 << "num_search_workers:4 "
                 << "log_search_progress:true "
                 << "cp_model_presolve:true "
                 << "stop_after_first_solution:false";
    sat_model.Add(NewSatParameters(param_stream.str()));
    auto response = SolveCpModel(model.Build(), &sat_model);
    
    cout << "[CPSatScheduler] Status: " << CpSolverStatus_Name(response.status()) << endl;
    cout << "[CPSatScheduler] CP-SAT objective (course_pref + time_pref only): " << response.objective_value() << endl;
    
    OptimalSolution sol;
    if (response.status() == CpSolverStatus::FEASIBLE || response.status() == CpSolverStatus::OPTIMAL)
    {
        extract_solution_to_optimal(response, Y, Y_room, data, R, sol);
        
        // Calculate full objective value (same as phase3: course_pref + time_pref - workload_penalty - compactness_penalty)
        const double w_course_pref = 1.0;
        const double w_time_pref = 1.0;
        const double w_workload_balance = 5.0;
        const double w_compactness = 3.0;
        
        double score = 0.0;
        
        // Course preference
        for (const auto &a : sol.assignments)
        {
            for (const auto &t : data.teachers)
            {
                if (t.id == a.teacher_id)
                {
                    auto pc_it = t.course_pref.find(a.course_id);
                    if (pc_it != t.course_pref.end())
                        score += w_course_pref * pc_it->second;
                    break;
                }
            }
        }
        
        // Time preference
        for (const auto &a : sol.assignments)
        {
            int required_periods = get_required_periods(data, a.course_id, a.section_id);
            int start_period_idx = find_period_index(data.classrooms.periods, a.period);
            
            if (start_period_idx >= 0)
            {
                for (int t = 0; t < required_periods && start_period_idx + t < (int)data.classrooms.periods.size(); ++t)
                {
                    string period = data.classrooms.periods[start_period_idx + t];
                    string key = a.day + "|" + period;
                    
                    for (const auto &teacher : data.teachers)
                    {
                        if (teacher.id == a.teacher_id)
                        {
                            for (const auto &tp : teacher.time_pref)
                            {
                                if (tp.day == a.day && tp.period == period)
                                {
                                    score += w_time_pref * tp.score;
                                    break;
                                }
                            }
                            break;
                        }
                    }
                }
            }
        }
        
        unordered_map<string, int> teacher_workloads;
        for (const auto &a : sol.assignments)
        {
            int required_periods = get_required_periods(data, a.course_id, a.section_id);
            int start_period_idx = find_period_index(data.classrooms.periods, a.period);
            
            if (start_period_idx >= 0)
            {
                for (int t = 0; t < required_periods && start_period_idx + t < (int)data.classrooms.periods.size(); ++t)
                {
                    teacher_workloads[a.teacher_id] += 1;
                }
            }
        }
        
        if (!teacher_workloads.empty())
        {
            vector<int> workloads;
            for (const auto &tw : teacher_workloads)
                workloads.push_back(tw.second);
            
            double mean = accumulate(workloads.begin(), workloads.end(), 0.0) / workloads.size();
            double variance = 0.0;
            for (int w : workloads)
                variance += (w - mean) * (w - mean);
            double stddev = sqrt(variance / workloads.size());
            score -= w_workload_balance * stddev;
        }
        
        // Compactness penalty
        unordered_map<string, unordered_map<string, vector<int>>> teacher_day_periods;
        for (const auto &a : sol.assignments)
        {
            int required_periods = get_required_periods(data, a.course_id, a.section_id);
            int start_period_idx = find_period_index(data.classrooms.periods, a.period);
            
            if (start_period_idx >= 0)
            {
                for (int t = 0; t < required_periods && start_period_idx + t < (int)data.classrooms.periods.size(); ++t)
                {
                    int period_idx = start_period_idx + t;
                    teacher_day_periods[a.teacher_id][a.day].push_back(period_idx);
                }
            }
        }
        
        double compactness_penalty = 0.0;
        for (const auto &teacher_entry : teacher_day_periods)
        {
            for (const auto &day_entry : teacher_entry.second)
            {
                vector<int> periods = day_entry.second;
                if (periods.empty()) continue;
                
                sort(periods.begin(), periods.end());
                periods.erase(unique(periods.begin(), periods.end()), periods.end());
                
                for (size_t i = 0; i < periods.size() - 1; ++i)
                {
                    int gap = periods[i + 1] - periods[i];
                    if (gap > 1)
                        compactness_penalty += (gap - 1);
                }
            }
        }
        score -= w_compactness * compactness_penalty;
        
        sol.objective_value = score;
        
        // Calculate breakdown for logging
        double course_time_score = 0.0;
        for (const auto &a : sol.assignments)
        {
            for (const auto &t : data.teachers)
            {
                if (t.id == a.teacher_id)
                {
                    auto pc_it = t.course_pref.find(a.course_id);
                    if (pc_it != t.course_pref.end())
                        course_time_score += w_course_pref * pc_it->second;
                    break;
                }
            }
        }
        for (const auto &a : sol.assignments)
        {
            int required_periods = get_required_periods(data, a.course_id, a.section_id);
            int start_period_idx = find_period_index(data.classrooms.periods, a.period);
            if (start_period_idx >= 0)
            {
                for (int t = 0; t < required_periods && start_period_idx + t < (int)data.classrooms.periods.size(); ++t)
                {
                    string period = data.classrooms.periods[start_period_idx + t];
                    for (const auto &teacher : data.teachers)
                    {
                        if (teacher.id == a.teacher_id)
                        {
                            for (const auto &tp : teacher.time_pref)
                            {
                                if (tp.day == a.day && tp.period == period)
                                {
                                    course_time_score += w_time_pref * tp.score;
                                    break;
                                }
                            }
                            break;
                        }
                    }
                }
            }
        }
        
        double workload_penalty_value = 0.0;
        if (!teacher_workloads.empty())
        {
            vector<int> workloads;
            for (const auto &tw : teacher_workloads)
                workloads.push_back(tw.second);
            double mean = accumulate(workloads.begin(), workloads.end(), 0.0) / workloads.size();
            double variance = 0.0;
            for (int w : workloads)
                variance += (w - mean) * (w - mean);
            double stddev = sqrt(variance / workloads.size());
            workload_penalty_value = stddev;
        }
        
        cout << "[CPSatScheduler] Full objective value (with penalties): " << sol.objective_value << endl;
        cout << "[CPSatScheduler]   Breakdown:" << endl;
        cout << "[CPSatScheduler]     Course preference + Time preference: " << course_time_score << endl;
        cout << "[CPSatScheduler]     Workload penalty (stddev * " << w_workload_balance << "): " 
             << (w_workload_balance * workload_penalty_value) << endl;
        cout << "[CPSatScheduler]     Compactness penalty (" << w_compactness << " * gaps): " 
             << (w_compactness * compactness_penalty) << endl;
    }
    else
    {
        cout << "[CPSatScheduler] No feasible solution found." << endl;
    }
    
    return sol;
}

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

        OptimalSolution opt = solve_with_cpsat_all_constraints(data, time_limit);

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

