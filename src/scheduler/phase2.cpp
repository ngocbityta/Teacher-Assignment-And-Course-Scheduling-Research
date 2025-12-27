#include "phase2.h"
#include "ortools/sat/cp_model.h"
#include "ortools/sat/cp_model_solver.h"
#include <iostream>
#include <vector>
#include <map>
#include <tuple>
#include <algorithm>
#include <random>
#include <chrono>
#include <unordered_set>
#include <unordered_map>
#include <cmath>
#include <sstream>

using namespace operations_research;
using namespace operations_research::sat;
using namespace std;

static int get_required_seats(const ProblemData &data, const string &course_id, const string &section_id)
{
    for (const auto &c : data.courses)
        if (c.id == course_id)
            for (const auto &s : c.sections)
                if (s.id == section_id)
                    return s.required_seats;
    return 0;
}

static int get_required_periods_for_eval(const ProblemData &data, const string &course_id, const string &section_id)
{
    for (const auto &c : data.courses)
        if (c.id == course_id)
            for (const auto &s : c.sections)
                if (s.id == section_id)
                    return s.required_periods;
    return 1;
}

static void extract_solution(const CpSolverResponse &response, const map<tuple<int,int,int,int,int>, BoolVar> &Y,
                             const map<tuple<int,int,int,int,int,int>, BoolVar> &Y_room, const ProblemData &data,
                             int R, InitialSolution &sol)
{
    for (auto &kv : Y)
    {
        if (SolutionBooleanValue(response, kv.second))
        {
            int i,j,k,l,m0;
            tie(i,j,k,l,m0) = kv.first;
            InitialSolution::Assignment a;
            a.teacher_id = data.teachers[i].id;
            a.course_id = data.courses[j].id;
            a.section_id = data.courses[j].sections[k].id;
            a.day = data.classrooms.days[l];
            a.period = data.classrooms.periods[m0];
            a.classroom_id = "";
            
            a.initial_teacher = a.teacher_id;
            a.initial_timeslot = a.day + "|" + a.period;
            
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

static double compute_diversity(const InitialSolution &sol1, const InitialSolution &sol2)
{
    if (sol1.assignments.size() != sol2.assignments.size())
        return 1.0;
    
    if (sol1.assignments.empty())
        return 0.0;
    
    map<pair<string, string>, pair<string, pair<string, string>>> map1, map2;
    
    for (const auto &a : sol1.assignments)
        map1[{a.course_id, a.section_id}] = {a.teacher_id, {a.day, a.period}};
    
    for (const auto &a : sol2.assignments)
        map2[{a.course_id, a.section_id}] = {a.teacher_id, {a.day, a.period}};
    
    int total = 0;
    int teacher_diff = 0;
    int timeslot_diff = 0;
    
    for (const auto &kv : map1)
    {
        auto it = map2.find(kv.first);
        if (it == map2.end())
        {
            total++;
            teacher_diff++;
            timeslot_diff++;
            continue;
        }
        
        total++;
        if (kv.second.first != it->second.first)
            teacher_diff++;
        if (kv.second.second != it->second.second)
            timeslot_diff++;
    }
    
    if (total == 0)
        return 0.0;
    
    double teacher_diversity = (double)teacher_diff / total;
    double timeslot_diversity = (double)timeslot_diff / total;
    
    return 0.5 * teacher_diversity + 0.5 * timeslot_diversity;
}

static bool is_solution_diverse_enough(
    const InitialSolution &sol, 
    const vector<InitialSolution> &pool,
    double min_diversity = 0.1)
{
    for (const auto &prev : pool)
    {
        double diversity = compute_diversity(sol, prev);
        if (diversity < min_diversity)
            return false;
    }
    return true;
}

InitialSolution construct_initial_solution(
    const ProblemData &data,
    int random_seed,
    bool shuffle_sections,
    bool shuffle_teachers,
    bool shuffle_timeslots)
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

    Model sat_model;
    ostringstream param_stream;
    param_stream << "max_time_in_seconds:60 "
                 << "num_search_workers:1 "
                 << "log_search_progress:false "
                 << "cp_model_presolve:true "
                 << "stop_after_first_solution:true "
                 << "random_seed:" << random_seed;
    sat_model.Add(NewSatParameters(param_stream.str()));
    auto response = SolveCpModel(model.Build(), &sat_model);
    
    cout << "[Phase2] Status: " << CpSolverStatus_Name(response.status()) << endl;
    
    if (response.status() == CpSolverStatus::FEASIBLE || response.status() == CpSolverStatus::OPTIMAL)
    {
        InitialSolution sol;
        extract_solution(response, Y, Y_room, data, R, sol);
        cout << "[Phase2] Found feasible solution (seed=" << random_seed 
             << ", leaving optimization space for Phase 3)" << endl;
        return sol;
    }
    
    cout << "[Phase2] No feasible solution found (seed=" << random_seed << ").\n";
    return InitialSolution();
}

static double compute_adaptive_diversity_threshold(int num_sections)
{
    if (num_sections <= 10)
        return 0.15;
    else if (num_sections <= 30)
        return 0.1;
    else
        return 0.05;
}

vector<InitialSolution> generate_solution_pool(
    const ProblemData &data,
    int K,
    double min_diversity,
    int max_solutions)
{
    vector<InitialSolution> pool;
    mt19937 rng(chrono::steady_clock::now().time_since_epoch().count());
    uniform_int_distribution<int> seed_dist(1, 1000000);
    
    if (min_diversity < 0)
    {
        int total_sections = 0;
        for (const auto &c : data.courses)
            total_sections += (int)c.sections.size();
        min_diversity = compute_adaptive_diversity_threshold(total_sections);
    }
    
    cout << "[Phase2] Generating solution pool (K=" << K 
         << ", min_diversity=" << min_diversity 
         << ", max_solutions=" << max_solutions << ")\n";
    
    int attempts = 0;
    int found = 0;
    
    while (attempts < K && (int)pool.size() < max_solutions)
    {
        attempts++;
        int random_seed = seed_dist(rng);
        
        cout << "[Phase2] Attempt " << attempts << "/" << K 
             << " (seed=" << random_seed << ")... ";
        
        InitialSolution sol = construct_initial_solution(data, random_seed);
        
        if (sol.assignments.empty())
        {
            cout << "No feasible solution\n";
            continue;
        }
        
        found++;
        
        if (is_solution_diverse_enough(sol, pool, min_diversity))
        {
            pool.push_back(sol);
            cout << "Added to pool (diversity OK, pool_size=" << pool.size() << ")\n";
        }
        else
        {
            cout << "Rejected (too similar to existing solutions)\n";
        }
    }
    
    cout << "[Phase2] Pool generation complete: " << found << " feasible found, "
         << pool.size() << " diverse solutions in pool\n";
    
    return pool;
}

vector<InitialSolution> select_diverse_solutions(
    const vector<InitialSolution> &pool,
    int N)
{
    if (pool.empty() || N <= 0)
        return {};
    
    if ((int)pool.size() <= N)
        return pool;
    
    vector<InitialSolution> selected;
    vector<bool> used(pool.size(), false);
    
    mt19937 rng(chrono::steady_clock::now().time_since_epoch().count());
    uniform_int_distribution<int> dist(0, (int)pool.size() - 1);
    int first_idx = dist(rng);
    selected.push_back(pool[first_idx]);
    used[first_idx] = true;
    
    for (int i = 1; i < N && (int)selected.size() < N; ++i)
    {
        int best_idx = -1;
        double best_score = -1.0;
        
        for (size_t j = 0; j < pool.size(); ++j)
        {
            if (used[j])
                continue;
            
            double total_diversity = 0.0;
            for (const auto &sel : selected)
            {
                total_diversity += compute_diversity(pool[j], sel);
            }
            
            if (total_diversity > best_score)
            {
                best_score = total_diversity;
                best_idx = j;
            }
        }
        
        if (best_idx >= 0)
        {
            selected.push_back(pool[best_idx]);
            used[best_idx] = true;
        }
    }
    
    return selected;
}

InitialSolution assign_rooms_phase2(const ProblemData &data, const InitialSolution &initial)
{
    InitialSolution result = initial;
    mt19937 rng(chrono::steady_clock::now().time_since_epoch().count());
    
    for (auto &a : result.assignments)
        if (a.classroom_id.empty())
        {
            int seats = get_required_seats(data, a.course_id, a.section_id);
            
            vector<string> feasible_rooms;
            for (const auto &room : data.classrooms.classrooms)
                if (room.capacity >= seats)
                    feasible_rooms.push_back(room.id);
            
            if (!feasible_rooms.empty())
            {
                uniform_int_distribution<int> dist(0, (int)feasible_rooms.size() - 1);
                a.classroom_id = feasible_rooms[dist(rng)];
            }
        }
    return result;
}

static double compute_stddev_simple(const vector<int> &vals)
{
    if (vals.empty())
        return 0.0;
    double mean = 0.0;
    for (int v : vals)
        mean += v;
    mean /= vals.size();
    
    double variance = 0.0;
    for (int v : vals)
        variance += (v - mean) * (v - mean);
    variance /= vals.size();
    
    return sqrt(variance);
}

double evaluate_phase2_solution_quick(
    const InitialSolution &sol,
    const ProblemData &data)
{
    if (sol.assignments.empty())
        return 0.0;
    
    unordered_map<string, int> teacher_periods;
    
    for (const auto &a : sol.assignments)
    {
        int required_periods = get_required_periods_for_eval(data, a.course_id, a.section_id);
        teacher_periods[a.teacher_id] += required_periods;
    }
    
    if (teacher_periods.empty())
        return 0.0;
    
    vector<int> workloads;
    for (const auto &tp : teacher_periods)
        workloads.push_back(tp.second);
    
    double workload_stddev = compute_stddev_simple(workloads);
    double max_workload = *max_element(workloads.begin(), workloads.end());
    
    double workload_score = 0.0;
    if (max_workload > 0)
        workload_score = max(0.0, 1.0 - (workload_stddev / max_workload));
    else
        workload_score = 1.0;
    
    unordered_set<string> used_timeslots;
    for (const auto &a : sol.assignments)
    {
        string timeslot = a.day + "|" + a.period;
        used_timeslots.insert(timeslot);
    }
    
    int total_timeslots = data.classrooms.days.size() * data.classrooms.periods.size();
    if (total_timeslots == 0)
        return 0.0;
    
    double utilization = (double)used_timeslots.size() / total_timeslots;
    double timeslot_score = 0.0;
    if (utilization < 0.3)
        timeslot_score = utilization / 0.3;
    else if (utilization <= 0.7)
        timeslot_score = 1.0;
    else
        timeslot_score = 1.0 - (utilization - 0.7) / 0.3;
    
    timeslot_score = max(0.0, min(1.0, timeslot_score));
    
    double final_score = 0.6 * workload_score + 0.4 * timeslot_score;
    
    return final_score;
}

bool is_phase2_solution_good_enough(
    const InitialSolution &sol,
    const ProblemData &data,
    double min_score)
{
    double score = evaluate_phase2_solution_quick(sol, data);
    return score >= min_score;
}
