#include "phase2.h"
#include "ortools/sat/cp_model.h"
#include "ortools/sat/cp_model_solver.h"
#include <iostream>
#include <map>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <algorithm>

using namespace operations_research;
using namespace operations_research::sat;
using namespace std;

void print_initial_solution(const InitialSolution &sol)
{
    if (sol.assignments.empty())
    {
        cout << "No assignments found in Phase2.\n";
        return;
    }

    cout << "Phase2 Initial Solution:\n";
    for (const auto &a : sol.assignments)
    {
        cout << "Teacher: " << a.teacher_id
             << ", Course: " << a.course_id
             << ", Section: " << a.section_id
             << ", Day: " << a.day
             << ", Start period: " << a.period << "\n";
    }
}

InitialSolution construct_initial_solution(const ProblemData &data)
{
    CpModelBuilder model;

    // ---------- indices and sizes ----------
    int I = (int)data.teachers.size();
    int J = (int)data.courses.size();
    int L = (int)data.classrooms.days.size();
    int M = (int)data.classrooms.periods.size();
    int C = (int)data.classrooms.classrooms.size(); // số lượng lớp học

    // sections per course
    vector<int> S;
    S.reserve(J);
    for (int j = 0; j < J; ++j)
        S.push_back((int)data.courses[j].sections.size());

    // maps id -> index (useful for building outputs)
    unordered_map<string, int> teacher_idx;
    unordered_map<string, int> course_idx;
    for (int i = 0; i < I; ++i)
        teacher_idx[data.teachers[i].id] = i;
    for (int j = 0; j < J; ++j)
        course_idx[data.courses[j].id] = j;

    // Build teacher time preference lookup: PT[i][l][m]
    vector<vector<vector<int>>> PT(I, vector<vector<int>>(L, vector<int>(M, 0)));
    for (int i = 0; i < I; ++i)
    {
        for (const auto &tp : data.teachers[i].time_pref)
        {
            int li = -1, mi = -1;
            for (int l = 0; l < L; ++l)
                if (data.classrooms.days[l] == tp.day)
                {
                    li = l;
                    break;
                }
            for (int m = 0; m < M; ++m)
                if (data.classrooms.periods[m] == tp.period)
                {
                    mi = m;
                    break;
                }
            if (li >= 0 && mi >= 0)
                PT[i][li][mi] = tp.score;
        }
    }

    // Course preference PC[i][j]
    vector<vector<int>> PC(I, vector<int>(J, 0));
    for (int i = 0; i < I; ++i)
    {
        for (int j = 0; j < J; ++j)
        {
            const auto &cid = data.courses[j].id;
            if (data.teachers[i].course_pref.count(cid))
                PC[i][j] = data.teachers[i].course_pref.at(cid);
        }
    }

    // Helper: teacher eligible table
    vector<vector<bool>> eligible(I, vector<bool>(J, false));
    for (int i = 0; i < I; ++i)
    {
        for (int j = 0; j < J; ++j)
        {
            if (find(data.teachers[i].eligible_courses.begin(), data.teachers[i].eligible_courses.end(),
                     data.courses[j].id) != data.teachers[i].eligible_courses.end())
            {
                eligible[i][j] = true;
            }
        }
    }

    // ---------- Variables ----------
    // Y(i,j,k,l,m0) : teacher i starts section k of course j at day l, starting period m0
    // We'll store in map keyed by tuple(i,j,k,l,m0)
    using Key = tuple<int, int, int, int, int>;
    map<Key, BoolVar> Y;

    for (int i = 0; i < I; ++i)
    {
        for (int j = 0; j < J; ++j)
        {
            if (!eligible[i][j])
                continue;
            for (int k = 0; k < S[j]; ++k)
            {
                int r = data.courses[j].sections[k].required_periods;
                for (int l = 0; l < L; ++l)
                {
                    for (int m0 = 0; m0 < M; ++m0)
                    {
                        if (m0 + r - 1 < M)
                        {
                            string name = string("Y_t") + to_string(i) + "_c" + to_string(j) + "_s" + to_string(k) + "_d" + to_string(l) + "_m" + to_string(m0);
                            Y[Key(i, j, k, l, m0)] = model.NewBoolVar().WithName(name);
                            ;
                        }
                        // else m0 invalid start -> no var created
                    }
                }
            }
        }
    }

    // P(i,j) : teacher i teaches course j (binary), create only for eligible combos
    map<pair<int, int>, BoolVar> P;
    for (int i = 0; i < I; ++i)
        for (int j = 0; j < J; ++j)
            if (eligible[i][j])
            {
                string name = "P_t" + to_string(i) + "_c" + to_string(j);
                P[{i, j}] = model.NewBoolVar().WithName(name);
            }

    // Z(i,j,k,l,m0,c) : teacher i starts section k of course j at day l, starting period m0, assigned to classroom c
    // Key: (i, j, k, l, m0, c)
    using ZKey = tuple<int, int, int, int, int, int>;
    map<ZKey, BoolVar> Z;

    for (int i = 0; i < I; ++i)
    {
        for (int j = 0; j < J; ++j)
        {
            if (!eligible[i][j])
                continue;
            for (int k = 0; k < S[j]; ++k)
            {
                int r = data.courses[j].sections[k].required_periods;
                int required_seats = data.courses[j].sections[k].required_seats;
                for (int l = 0; l < L; ++l)
                {
                    for (int m0 = 0; m0 < M; ++m0)
                    {
                        if (m0 + r - 1 < M)
                        {
                            // Chỉ tạo biến Z cho các lớp học có đủ capacity
                            for (int c = 0; c < C; ++c)
                            {
                                if (data.classrooms.classrooms[c].capacity >= required_seats)
                                {
                                    string name = string("Z_t") + to_string(i) + "_c" + to_string(j) + "_s" + to_string(k) + "_d" + to_string(l) + "_m" + to_string(m0) + "_room" + to_string(c);
                                    Z[ZKey(i, j, k, l, m0, c)] = model.NewBoolVar().WithName(name);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // ---------- Constraints ----------

    // 1) Each section must be scheduled exactly once (one teacher, one day, one start)
    for (int j = 0; j < J; ++j)
    {
        for (int k = 0; k < S[j]; ++k)
        {
            LinearExpr sumStarts;
            for (int i = 0; i < I; ++i)
            {
                if (!eligible[i][j])
                    continue;
                for (int l = 0; l < L; ++l)
                    for (int m0 = 0; m0 < M; ++m0)
                    {
                        auto it = Y.find({i, j, k, l, m0});
                        if (it != Y.end())
                            sumStarts += it->second;
                    }
            }
            model.AddEquality(sumStarts, 1);
        }
    }

    // 1a) Link Y and Z: if Y(i,j,k,l,m0) = 1, then exactly one Z(i,j,k,l,m0,c) = 1
    for (auto &y_entry : Y)
    {
        Key y_key = y_entry.first;
        int i, j, k, l, m0;
        tie(i, j, k, l, m0) = y_key;
        int required_seats = data.courses[j].sections[k].required_seats;
        
        LinearExpr sumZ = 0;
        for (int c = 0; c < C; ++c)
        {
            if (data.classrooms.classrooms[c].capacity >= required_seats)
            {
                auto z_it = Z.find({i, j, k, l, m0, c});
                if (z_it != Z.end())
                    sumZ += z_it->second;
            }
        }
        // Y[i,j,k,l,m0] == sum_c Z[i,j,k,l,m0,c]
        model.AddEquality(y_entry.second, sumZ);
    }

    // 2) Link P and Y: if any Y(i,j,k,.,.) = 1 => P(i,j) = 1, and if P=1 then sumY >= 1
    for (int i = 0; i < I; ++i)
        for (int j = 0; j < J; ++j)
            if (eligible[i][j])
            {
                LinearExpr sumY_ij;
                for (int k = 0; k < S[j]; ++k)
                    for (int l = 0; l < L; ++l)
                        for (int m0 = 0; m0 < M; ++m0)
                        {
                            auto it = Y.find({i, j, k, l, m0});
                            if (it != Y.end())
                                sumY_ij += it->second;
                        }
                // sumY_ij <= S[j] * P[i,j]
                model.AddLessOrEqual(sumY_ij, LinearExpr(P[{i, j}]) * S[j]);
                // sumY_ij >= P[i,j]
                model.AddGreaterOrEqual(sumY_ij, P[{i, j}]);
            }

    // 3) Teacher max/min courses: sum_j P[i,j] <= max_courses, each teacher must teach >=1
    for (int i = 0; i < I; ++i)
    {
        LinearExpr sumP;
        for (int j = 0; j < J; ++j)
            if (eligible[i][j])
                sumP += P[{i, j}];
        model.AddGreaterOrEqual(sumP, 1);
        model.AddLessOrEqual(sumP, data.teachers[i].max_courses);
    }

    // 4) Each course must be taught by at least min_teachers, at most max_teachers
    for (int j = 0; j < J; ++j)
    {
        LinearExpr sum_teachers = 0;
        for (int i = 0; i < I; ++i)
        {
            if (eligible[i][j])
                sum_teachers += P[{i, j}];
        }
        model.AddGreaterOrEqual(sum_teachers, data.courses[j].min_teachers);
        model.AddLessOrEqual(sum_teachers, data.courses[j].max_teachers);
    }

    // 5) Classroom capacity & teacher single-slot & course-per-time constraints:
    // For every (l,m) and every classroom c, we compute occupancy by summing Z that cover (l,m) and use classroom c
    for (int l = 0; l < L; ++l)
        for (int m = 0; m < M; ++m)
        {
            // For course-per-slot restriction: for each course j ensure at most 1 section of this course in (l,m)
            vector<LinearExpr> per_course_sum(J);
            for (int j = 0; j < J; ++j)
                per_course_sum[j] = LinearExpr(0);

            // For each classroom, check capacity constraint
            for (int c = 0; c < C; ++c)
            {
                LinearExpr total_in_classroom_slot = 0;
                for (int i = 0; i < I; ++i)
                {
                    for (int j = 0; j < J; ++j)
                    {
                        if (!eligible[i][j])
                            continue;
                        for (int k = 0; k < S[j]; ++k)
                        {
                            int r = data.courses[j].sections[k].required_periods;
                            // any start m0 that covers m: m0 <= m <= m0 + r - 1
                            for (int m0 = max(0, m - r + 1); m0 <= m; ++m0)
                            {
                                auto z_it = Z.find({i, j, k, l, m0, c});
                                if (z_it != Z.end())
                                {
                                    total_in_classroom_slot += z_it->second;
                                    per_course_sum[j] += z_it->second;
                                }
                            }
                        }
                    }
                }
                // Mỗi lớp học chỉ có thể được sử dụng bởi tối đa 1 section tại mỗi time slot
                model.AddLessOrEqual(total_in_classroom_slot, 1);
            }

            // per course per slot <= 1 (tính từ Y để đảm bảo không có 2 section cùng course trong cùng slot)
            LinearExpr total_in_slot = 0;
            for (int i = 0; i < I; ++i)
            {
                for (int j = 0; j < J; ++j)
                {
                    if (!eligible[i][j])
                        continue;
                    for (int k = 0; k < S[j]; ++k)
                    {
                        int r = data.courses[j].sections[k].required_periods;
                        for (int m0 = max(0, m - r + 1); m0 <= m; ++m0)
                        {
                            auto it = Y.find({i, j, k, l, m0});
                            if (it != Y.end())
                            {
                                total_in_slot += it->second;
                            }
                        }
                    }
                }
            }
            
            // Fallback: nếu không có classrooms được định nghĩa, sử dụng Clm (tương thích ngược)
            if (C == 0 && !data.classrooms.Clm.empty())
            {
                auto day_it = data.classrooms.Clm.find(data.classrooms.days[l]);
                if (day_it != data.classrooms.Clm.end())
                {
                    auto period_it = day_it->second.find(data.classrooms.periods[m]);
                    if (period_it != day_it->second.end())
                    {
                        int cap = period_it->second;
                        model.AddLessOrEqual(total_in_slot, cap);
                    }
                }
            }

            // per course per slot <= 1
            for (int j = 0; j < J; ++j)
            {
                model.AddLessOrEqual(per_course_sum[j], 1);
            }
        }

    // 6) Each teacher at most 1 section per time slot (enforce by summing Y that cover slot for that teacher)
    for (int i = 0; i < I; ++i)
    {
        for (int l = 0; l < L; ++l)
            for (int m = 0; m < M; ++m)
            {
                LinearExpr teacher_slot = 0;
                for (int j = 0; j < J; ++j)
                {
                    if (!eligible[i][j])
                        continue;
                    for (int k = 0; k < S[j]; ++k)
                    {
                        int r = data.courses[j].sections[k].required_periods;
                        for (int m0 = max(0, m - r + 1); m0 <= m; ++m0)
                        {
                            auto it = Y.find({i, j, k, l, m0});
                            if (it != Y.end())
                                teacher_slot += it->second;
                        }
                    }
                }
                model.AddLessOrEqual(teacher_slot, 1);
            }
    }

    // 7) Each teacher schedule should be spread evenly over days (add penalty when overloaded)

    vector<int> total_sections(I, 0);
    for (int i = 0; i < I; ++i)
    {
        for (int j = 0; j < J; ++j)
        {
            if (!eligible[i][j])
                continue;
            total_sections[i] += S[j];
        }
    }

    map<pair<int, int>, IntVar> overload;
    for (int i = 0; i < I; ++i)
    {
        int avg = (total_sections[i] + L - 1) / L;
        for (int l = 0; l < L; ++l)
        {
            LinearExpr sections_on_day = 0;
            for (int j = 0; j < J; ++j)
            {
                if (!eligible[i][j])
                    continue;
                for (int k = 0; k < S[j]; ++k)
                    for (int m0 = 0; m0 < M; ++m0)
                    {
                        auto it = Y.find({i, j, k, l, m0});
                        if (it != Y.end())
                            sections_on_day += it->second;
                    }
            }

            Domain d = Domain(0, total_sections[i]);

            overload[{i, l}] = model.NewIntVar(d).WithName(
                "overload_t" + to_string(i) + "_d" + to_string(l));
            model.AddGreaterOrEqual(overload[{i, l}], sections_on_day - avg);
        }
    }

    // ---------- Objective ----------
    // Maximize sum(PC[i][j] * P[i][j]) + sum_over_Y (sum_{t in covered periods} PT[i][l][t]) * Y
    LinearExpr objective;
    for (const auto &entry : P)
    {
        int i = entry.first.first;
        int j = entry.first.second;
        objective += LinearExpr(entry.second) * PC[i][j];
    }
    // time-pref contribution: for each Y, sum PT over each occupied period and weight by Y
    for (auto &it : Y)
    {
        Key key = it.first;
        int i, j, k, l, m0;
        tie(i, j, k, l, m0) = key;
        int r = data.courses[j].sections[k].required_periods;
        int sumPT = 0;
        for (int t = 0; t < r; ++t)
        {
            int m = m0 + t;
            sumPT += PT[i][l][m];
        }
        objective += LinearExpr(it.second) * sumPT;
    }
    for (auto &entry : overload)
    {
        objective -= entry.second;
    }

    model.Maximize(objective);

    // ---------- Solve with solver parameters ----------
    Model sat_model;
    sat_model.Add(NewSatParameters("max_time_in_seconds:30 num_search_workers:8 log_search_progress: false"));

    auto response = SolveCpModel(model.Build(), &sat_model);

    cout << "Phase2 solver status: " << CpSolverStatus_Name(response.status()) << "\n";

    InitialSolution sol;
    if (response.status() == CpSolverStatus::FEASIBLE || response.status() == CpSolverStatus::OPTIMAL)
    {
        // Extract assignments from Z (classroom assignment vars) to get both schedule and classroom
        for (auto &entry : Z)
        {
            ZKey key = entry.first;
            const BoolVar &var = entry.second;
            if (SolutionBooleanValue(response, var))
            {
                int i, j, k, l, m0, c;
                tie(i, j, k, l, m0, c) = key;
                InitialSolution::Assignment a;
                a.teacher_id = data.teachers[i].id;
                a.course_id = data.courses[j].id;
                a.section_id = data.courses[j].sections[k].id;
                a.day = data.classrooms.days[l];
                a.period = data.classrooms.periods[m0]; // start period
                a.classroom_id = data.classrooms.classrooms[c].id;
                sol.assignments.push_back(a);
            }
        }
    }
    else
    {
        cout << "No feasible solution found in Phase2.\n";
    }

    return sol;
}
