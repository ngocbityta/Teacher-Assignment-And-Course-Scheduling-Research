#include "CPSatSolver.h"
#include <numeric>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <sstream> // Include sstream for ostringstream

using namespace std;

CPSatSolver::CPSatSolver(const ProblemData& data, int time_limit_seconds)
    : data_(data), time_limit_seconds_(time_limit_seconds)
{
}

OptimalSolution CPSatSolver::solve()
{
    create_variables();
    add_hard_constraints();
    add_soft_constraints();
    
    // Solve with optimized parameters
    Model sat_model;
    ostringstream param_stream;
    param_stream << "max_time_in_seconds:" << time_limit_seconds_ << " "
                 << "num_search_workers:8 "           // Use 8 workers for parallelism
                 << "log_search_progress:false "      // Disable verbose logging
                 << "cp_model_presolve:true "
                 << "linearization_level:2 "          // More aggressive linearization
                 << "stop_after_first_solution:false";
    sat_model.Add(NewSatParameters(param_stream.str()));
    
    auto response = SolveCpModel(model_.Build(), &sat_model);
    
    OptimalSolution sol;
    if (response.status() == CpSolverStatus::FEASIBLE || response.status() == CpSolverStatus::OPTIMAL)
    {
        sol = extract_solution(response);
    }
    
    return sol;
}

void CPSatSolver::create_variables()
{
    int I = (int)data_.teachers.size();
    int J = (int)data_.courses.size();
    int L = (int)data_.classrooms.days.size();
    int M = (int)data_.classrooms.periods.size();
    int R = (int)data_.classrooms.classrooms.size();
    
    // Determine eligible assignments
    vector<vector<bool>> eligible(I, vector<bool>(J, false));
    for (int i = 0; i < I; ++i)
        for (int j = 0; j < J; ++j)
            eligible[i][j] = find(data_.teachers[i].eligible_courses.begin(),
                                  data_.teachers[i].eligible_courses.end(),
                                  data_.courses[j].id) != data_.teachers[i].eligible_courses.end();

    // Create decision variables
    for (int i = 0; i < I; ++i)
        for (int j = 0; j < J; ++j)
            if (eligible[i][j])
            {
                int S = (int)data_.courses[j].sections.size();
                for (int k = 0; k < S; ++k)
                {
                    int r = data_.courses[j].sections[k].required_periods;
                    int seats = data_.courses[j].sections[k].required_seats;
                    for (int l = 0; l < L; ++l)
                        for (int m0 = 0; m0 + r <= M; ++m0)
                        {
                            Y_[{i,j,k,l,m0}] = model_.NewBoolVar();
                            if (R > 0)
                                for (int room_idx = 0; room_idx < R; ++room_idx)
                                    if (data_.classrooms.classrooms[room_idx].capacity >= seats)
                                        Y_room_[{i,j,k,l,m0,room_idx}] = model_.NewBoolVar();
                        }
                }
            }
}

void CPSatSolver::add_hard_constraints()
{
    add_one_section_assignment_constraint();
    add_room_coupling_constraint();
    add_teacher_time_overlap_constraint();
    add_room_capacity_overlap_constraint();
    add_teacher_course_constraints();
    add_course_section_overlap_constraint();
}

void CPSatSolver::add_one_section_assignment_constraint()
{
    int I = (int)data_.teachers.size();
    int J = (int)data_.courses.size();
    int L = (int)data_.classrooms.days.size();
    int M = (int)data_.classrooms.periods.size();
    
    for (int j = 0; j < J; ++j)
    {
        int S = (int)data_.courses[j].sections.size();
        for (int k = 0; k < S; ++k)
        {
            LinearExpr sum = 0;
            // Iterate over all possible assignments for this section
            // Optimization: Only iterate over eligible teachers and valid time slots
            // But here we iterate all to match Y keys, Y lookup handles non-existence
            for (auto const& [key, var] : Y_)
            {
                if (get<1>(key) == j && get<2>(key) == k)
                    sum += var;
            }
            model_.AddEquality(sum, 1);
        }
    }
}

void CPSatSolver::add_room_coupling_constraint()
{
    int R = (int)data_.classrooms.classrooms.size();
    if (R == 0) return;
    
    for (auto const& [key, y_var] : Y_)
    {
        int i, j, k, l, m0;
        tie(i, j, k, l, m0) = key;
        
        LinearExpr room_sum = 0;
        for (int room_idx = 0; room_idx < R; ++room_idx)
        {
            auto it_room = Y_room_.find({i,j,k,l,m0,room_idx});
            if (it_room != Y_room_.end())
                room_sum += it_room->second;
        }
        model_.AddEquality(room_sum, y_var);
    }
}

void CPSatSolver::add_teacher_time_overlap_constraint()
{
    int I = (int)data_.teachers.size();
    int L = (int)data_.classrooms.days.size();
    int M = (int)data_.classrooms.periods.size();
    int J = (int)data_.courses.size();

    // Teacher cannot teach two things at same time
    for (int i = 0; i < I; ++i)
        for (int l = 0; l < L; ++l)
            for (int m = 0; m < M; ++m)
            {
                LinearExpr load = 0;
                // Optimization: Iterate over Y_ directly is inefficient here.
                // Better loop over courses/sections and check time intervals
                
                // Reconstruct logic from original:
                // Identify all (j,k) and start times tm0 such that [tm0, tm0+r) overlaps m
                for (int j = 0; j < J; ++j)
                {
                    int S = (int)data_.courses[j].sections.size();
                    for (int k = 0; k < S; ++k)
                    {
                        int r = data_.courses[j].sections[k].required_periods;
                        // Start times m0 that cover period m: max(0, m - r + 1) <= m0 <= m
                        int start_m0 = max(0, m - r + 1);
                        int end_m0 = m;
                        
                        for (int m0 = start_m0; m0 <= end_m0; ++m0)
                        {
                            auto it = Y_.find({i,j,k,l,m0});
                            if (it != Y_.end())
                                load += it->second;
                        }
                    }
                }
                model_.AddLessOrEqual(load, 1);
            }
}

void CPSatSolver::add_room_capacity_overlap_constraint()
{
    int R = (int)data_.classrooms.classrooms.size();
    if (R == 0) {
        // No rooms: check if any section needs more seats than max capacity (if there was a theoretical max)
        // But here we rely on initialization not creating Y_room if capacity is insufficient
        // The Y variables are created regardless of room capacity, BUT if R=0 we don't assign rooms.
        // Original code had a check "if (R > 0) ... else ...".
        // In "else", it checked if required_seats > max_cap_of_all_rooms -> set Y=0.
        // We can replicate that logic here if needed, but let's stick to room assignments.
        
        int max_cap = 0;
        for (auto &c : data_.classrooms.classrooms)
            max_cap = max(max_cap, c.capacity);
            
        if (max_cap > 0) { // Only if there are defined classrooms but R=0 (shouldn't happen if parsing is correct usually)
             // Or maybe R=0 means no classrooms passed?
             // If classrooms are passed, R > 0.
             // If R=0, we skip room logic.
        }
        return;
    }

    int I = (int)data_.teachers.size();
    int J = (int)data_.courses.size();
    int L = (int)data_.classrooms.days.size();
    int M = (int)data_.classrooms.periods.size();

    // 1. Invalid capacity handled during variable creation (Y_room not created).
    // But we need to force Y_room to 0 if we created it? No, if we didn't create it, we can't use it.
    // Wait, add_room_coupling_constraint sums Y_room. If Y_room doesn't exist for a room (due to capacity),
    // it won't be in the sum. If no room has capacity, sum is 0, so Y must be 0. Correct.

    // 2. Room double booking
    for (int room_idx = 0; room_idx < R; ++room_idx)
        for (int l = 0; l < L; ++l)
            for (int m = 0; m < M; ++m)
            {
                LinearExpr room_load = 0;
                for (int i = 0; i < I; ++i)
                    for (int j = 0; j < J; ++j)
                    {
                         int S = (int)data_.courses[j].sections.size();
                         for (int k = 0; k < S; ++k)
                         {
                            int r = data_.courses[j].sections[k].required_periods;
                            int start_m0 = max(0, m - r + 1);
                            int end_m0 = m;
                            
                            for (int m0 = start_m0; m0 <= end_m0; ++m0)
                            {
                                if (m0 + r > M) continue;
                                auto it_room = Y_room_.find({i,j,k,l,m0,room_idx});
                                if (it_room != Y_room_.end())
                                    room_load += it_room->second;
                            }
                         }
                    }
                model_.AddLessOrEqual(room_load, 1);
            }
}

void CPSatSolver::add_teacher_course_constraints()
{
    int I = (int)data_.teachers.size();
    int J = (int)data_.courses.size();
    int L = (int)data_.classrooms.days.size();
    int M = (int)data_.classrooms.periods.size();
    
    // Min/Max teachers per course
    for (int j = 0; j < J; ++j)
    {
        int min_t = data_.courses[j].min_teachers;
        int max_t = data_.courses[j].max_teachers;
        
        // Count eligible teachers to avoid unsatisfiable constraints
        int eligible_count = 0;
        for (int i = 0; i < I; ++i) {
             bool eligible = false;
             for (auto const& [key, var] : Y_) {
                 if (get<0>(key) == i && get<1>(key) == j) {
                     eligible = true;
                     break;
                 }
             }
             if (eligible) eligible_count++; // This is approximate, checking if any Y exists
        }
        // Better: check ProblemData eligible
        eligible_count = 0;
        for(int i=0; i<I; ++i) {
            bool is_elig = find(data_.teachers[i].eligible_courses.begin(), 
                              data_.teachers[i].eligible_courses.end(), 
                              data_.courses[j].id) != data_.teachers[i].eligible_courses.end();
            if(is_elig) eligible_count++;
        }

        if (min_t > 0 || (max_t > 0 && max_t < eligible_count))
        {
            map<int, BoolVar> teacher_indicator;
            for (int i = 0; i < I; ++i)
            {
                // Check eligibility
                 bool is_elig = find(data_.teachers[i].eligible_courses.begin(), 
                              data_.teachers[i].eligible_courses.end(), 
                              data_.courses[j].id) != data_.teachers[i].eligible_courses.end();
                if (!is_elig) continue;

                BoolVar indicator = model_.NewBoolVar();
                teacher_indicator[i] = indicator;
                LinearExpr sum = 0;
                
                int S = (int)data_.courses[j].sections.size();
                for (int k = 0; k < S; ++k)
                {
                    int r = data_.courses[j].sections[k].required_periods;
                    for (int l = 0; l < L; ++l)
                        for (int m0 = 0; m0 + r <= M; ++m0)
                        {
                            auto it = Y_.find({i,j,k,l,m0});
                            if (it != Y_.end())
                                sum += it->second;
                        }
                }
                model_.AddGreaterOrEqual(sum, indicator);
                // Upper bound to force indicator to 0 if sum is 0 (optional but good for consistency)
                // logic: indicator=1 => sum>=1. 
                // We also need sum > 0 => indicator = 1? Usually handled by minimization or tight constraints.
                // But for min/max teachers, we need accurate counting.
                // If sum > 0, indicator CAN be 0 (constraint satisfied). That's wrong.
                // We need sum > 0 <=> indicator = 1.
                // AddLessOrEqual(sum, bigM * indicator).
                model_.AddLessOrEqual(sum, S * L * M * indicator); 
            }
            
            LinearExpr total = 0;
            for (auto &kv : teacher_indicator)
                total += kv.second;
            if (min_t > 0) model_.AddGreaterOrEqual(total, min_t);
            if (max_t > 0 && max_t < eligible_count) model_.AddLessOrEqual(total, max_t);
        }
    }
    
    // Teacher Max Courses
    for (int i = 0; i < I; ++i)
    {
        map<int, BoolVar> course_indicators;
        for (int j = 0; j < J; ++j)
        {
             bool is_elig = find(data_.teachers[i].eligible_courses.begin(), 
                              data_.teachers[i].eligible_courses.end(), 
                              data_.courses[j].id) != data_.teachers[i].eligible_courses.end();
            if (!is_elig) continue;

            BoolVar teaches_course = model_.NewBoolVar();
            course_indicators[j] = teaches_course;
            LinearExpr section_sum = 0;
            
            int S = (int)data_.courses[j].sections.size();
            for (int k = 0; k < S; ++k)
            {
                int r = data_.courses[j].sections[k].required_periods;
                for (int l = 0; l < L; ++l)
                    for (int m0 = 0; m0 + r <= M; ++m0)
                    {
                        auto it = Y_.find({i,j,k,l,m0});
                        if (it != Y_.end())
                        {
                            section_sum += it->second;
                            model_.AddGreaterOrEqual(teaches_course, it->second);
                        }
                    }
            }
            if (S > 0)
                model_.AddLessOrEqual(section_sum, teaches_course * S * L * M);
        }
        if (!course_indicators.empty())
        {
            LinearExpr num_courses = 0;
            for (const auto &kv : course_indicators)
                num_courses += kv.second;
            model_.AddLessOrEqual(num_courses, data_.teachers[i].max_courses);
        }
    }
}

void CPSatSolver::add_course_section_overlap_constraint()
{
    int I = (int)data_.teachers.size();
    int J = (int)data_.courses.size();
    int L = (int)data_.classrooms.days.size();
    int M = (int)data_.classrooms.periods.size();
    
    for (int j = 0; j < J; ++j)
    {
        int S = (int)data_.courses[j].sections.size();
        if (S <= 1) continue;
        
        for (int l = 0; l < L; ++l)
            for (int m = 0; m < M; ++m)
            {
                LinearExpr sections = 0;
                for (int k = 0; k < S; ++k)
                {
                    int r = data_.courses[j].sections[k].required_periods;
                    int start_m0 = max(0, m - r + 1);
                    int end_m0 = m;
                    
                    for (int m0 = start_m0; m0 <= end_m0; ++m0)
                    {
                         if (m0 + r > M) continue;
                        // Sum over all teachers who could teach this
                        for (int i = 0; i < I; ++i)
                        {
                            auto it = Y_.find({i,j,k,l,m0});
                            if (it != Y_.end())
                                sections += it->second;
                        }
                    }
                }
                model_.AddLessOrEqual(sections, 1);
            }
    }
}

void CPSatSolver::add_soft_constraints()
{
     int I = (int)data_.teachers.size();
    int J = (int)data_.courses.size();
    int L = (int)data_.classrooms.days.size();
    int M = (int)data_.classrooms.periods.size();
    
    // Weights
    const double w_course_pref = 1.0;
    const double w_time_pref = 1.0;
    const double w_workload_balance = 5.0;
    const double w_compactness = 3.0;

    LinearExpr objective = 0;

    // 1. Course preference
    for (int i = 0; i < I; ++i)
        for (int j = 0; j < J; ++j)
        {
             bool is_elig = find(data_.teachers[i].eligible_courses.begin(), 
                              data_.teachers[i].eligible_courses.end(), 
                              data_.courses[j].id) != data_.teachers[i].eligible_courses.end();
            if (!is_elig) continue;

            int course_pref_score = 0;
            auto pc_it = data_.teachers[i].course_pref.find(data_.courses[j].id);
            if (pc_it != data_.teachers[i].course_pref.end())
                course_pref_score = pc_it->second;

            int S = (int)data_.courses[j].sections.size();
            for (int k = 0; k < S; ++k)
            {
                int r = data_.courses[j].sections[k].required_periods;
                for (int l = 0; l < L; ++l)
                    for (int m0 = 0; m0 + r <= M; ++m0)
                    {
                        auto it = Y_.find({i,j,k,l,m0});
                        if (it != Y_.end())
                            objective += w_course_pref * course_pref_score * it->second;
                    }
            }
        }

    // 2. Time preference
    for (int i = 0; i < I; ++i)
        for (int j = 0; j < J; ++j)
        {
             bool is_elig = find(data_.teachers[i].eligible_courses.begin(), 
                              data_.teachers[i].eligible_courses.end(), 
                              data_.courses[j].id) != data_.teachers[i].eligible_courses.end();
            if (!is_elig) continue;
            
             int S = (int)data_.courses[j].sections.size();
             for (int k = 0; k < S; ++k)
             {
                int r = data_.courses[j].sections[k].required_periods;
                for (int l = 0; l < L; ++l)
                    for (int m0 = 0; m0 + r <= M; ++m0)
                    {
                        auto it = Y_.find({i,j,k,l,m0});
                        if (it == Y_.end()) continue;

                        int time_pref_score = 0;
                        string day = data_.classrooms.days[l];
                        for (int t = 0; t < r && m0 + t < M; ++t)
                        {
                            string period = data_.classrooms.periods[m0 + t];
                            for (const auto &tp : data_.teachers[i].time_pref)
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
        }

    // 3. Workload balance
    map<int, LinearExpr> teacher_workloads;
    for (int i = 0; i < I; ++i)
    {
        LinearExpr workload = 0;
        // Count total periods assigned
        // Iterate all assignments
        for (auto const& [key, var] : Y_)
        {
             if (get<0>(key) == i)
             {
                 int j = get<1>(key);
                 int k = get<2>(key);
                 int r = data_.courses[j].sections[k].required_periods;
                 workload += r * var;
             }
        }
        teacher_workloads[i] = workload;
    }

    if (!teacher_workloads.empty() && I > 1)
    {
        IntVar max_workload = model_.NewIntVar(Domain(0, 10000));
        IntVar min_workload = model_.NewIntVar(Domain(0, 10000));
        
        for (auto &kv : teacher_workloads)
        {
            IntVar workload_var = model_.NewIntVar(Domain(0, 10000));
            model_.AddEquality(workload_var, kv.second);
            model_.AddGreaterOrEqual(max_workload, workload_var);
            model_.AddLessOrEqual(min_workload, workload_var);
        }
        
        IntVar workload_diff = model_.NewIntVar(Domain(0, 10000));
        model_.AddEquality(workload_diff, max_workload - min_workload);
        objective -= w_workload_balance * workload_diff;
    }

    // 4. Compactness
    for (int i = 0; i < I; ++i)
        for (int l = 0; l < L; ++l)
        {
            map<int, BoolVar> has_class;
            for (int m = 0; m < M; ++m)
            {
                has_class[m] = model_.NewBoolVar();
                LinearExpr sum = 0;
                // Check if teacher i has class at time l, m
                 for (int j = 0; j < J; ++j)
                {
                    int S = (int)data_.courses[j].sections.size();
                    for (int k = 0; k < S; ++k)
                    {
                        int r = data_.courses[j].sections[k].required_periods;
                        int start_m0 = max(0, m - r + 1);
                        int end_m0 = m;
                        for (int m0 = start_m0; m0 <= end_m0; ++m0)
                        {
                            if (m0 + r > M) continue;
                            auto it = Y_.find({i,j,k,l,m0});
                            if (it != Y_.end())
                                sum += it->second;
                        }
                    }
                }
                // has_class[m] <=> sum >= 1
                // 1. sum >= 1 => has_class (if there's a class, has_class must be 1)
                model_.AddGreaterOrEqual(has_class[m], sum);
                // 2. has_class => sum >= 1 (if has_class is 1, there must be a class)
                model_.AddLessOrEqual(has_class[m], sum);
                // 3. has_class is binary (0 or 1)
                model_.AddLessOrEqual(has_class[m], 1);
            }

            // New Compactness Logic: Minimize Empty Slots (Gap Size) - Aligned with Heuristic
            // An empty slot is a gap if:
            // 1. It has no class (has_class[m] == 0)
            // 2. There is a class before it (started_before[m] == 1)
            // 3. There is a class after it (class_after[m] == 1)
            
            std::vector<BoolVar> started_before(M);
            std::vector<BoolVar> class_after(M);
            
            // Forward pass for started_before
            for (int m = 0; m < M; ++m) {
                started_before[m] = model_.NewBoolVar();
                if (m == 0) {
                    model_.AddEquality(started_before[m], 0);
                } else {
                    // started_before[m] <=> started_before[m-1] OR has_class[m-1]
                    // 1. (start[m-1] OR class[m-1]) => start[m]
                    model_.AddImplication(started_before[m-1], started_before[m]);
                    model_.AddImplication(has_class[m-1], started_before[m]);
                    
                    // 2. start[m] => (start[m-1] OR class[m-1])
                    model_.AddBoolOr({started_before[m-1], has_class[m-1], started_before[m].Not()});
                }
            }
            
            // Backward pass for class_after
            for (int m = M - 1; m >= 0; --m) {
                class_after[m] = model_.NewBoolVar();
                if (m == M - 1) {
                    model_.AddEquality(class_after[m], 0);
                } else {
                    // class_after[m] <=> class_after[m+1] OR has_class[m+1]
                    // 1. (after[m+1] OR class[m+1]) => after[m]
                    model_.AddImplication(class_after[m+1], class_after[m]);
                    model_.AddImplication(has_class[m+1], class_after[m]);
                    
                    // 2. after[m] => (after[m+1] OR class[m+1])
                    model_.AddBoolOr({class_after[m+1], has_class[m+1], class_after[m].Not()});
                }
            }
            
            // Gap penalty: is_gap = 1 when slot is empty but surrounded by classes
            for (int m = 0; m < M; ++m) {
                BoolVar is_gap = model_.NewBoolVar();
                // Constraint: (!has_class && started_before && class_after) => is_gap
                model_.AddBoolOr({has_class[m], started_before[m].Not(), class_after[m].Not(), is_gap});
                objective -= w_compactness * is_gap;
            }
        }

    model_.Maximize(objective);
}

OptimalSolution CPSatSolver::extract_solution(const CpSolverResponse& response)
{
    OptimalSolution sol;
    int R = (int)data_.classrooms.classrooms.size();
    
    for (auto const& [key, var] : Y_)
    {
        if (SolutionBooleanValue(response, var))
        {
            int i,j,k,l,m0;
            tie(i,j,k,l,m0) = key;
            
            OptimalSolution::Assignment a;
            a.teacher_id = data_.teachers[i].id;
            a.course_id = data_.courses[j].id;
            a.section_id = data_.courses[j].sections[k].id;
            a.day = data_.classrooms.days[l];
            a.period = data_.classrooms.periods[m0];
            a.classroom_id = "";
            
            if (R > 0)
            {
                for (int room_idx = 0; room_idx < R; ++room_idx)
                {
                    auto it_room = Y_room_.find({i,j,k,l,m0,room_idx});
                    if (it_room != Y_room_.end() && SolutionBooleanValue(response, it_room->second))
                    {
                        a.classroom_id = data_.classrooms.classrooms[room_idx].id;
                        break;
                    }
                }
            }
            sol.assignments.push_back(a);
        }
    }
    
    // Use the same evaluation function as Heuristic for consistent comparison
    sol.objective_value = evaluate_solution(data_, sol);
    return sol;
}

int CPSatSolver::get_required_periods(const std::string &course_id, const std::string &section_id) const
{
     for (const auto &c : data_.courses)
        if (c.id == course_id)
            for (const auto &s : c.sections)
                if (s.id == section_id)
                    return s.required_periods;
    return 1;
}

int CPSatSolver::find_period_index(const std::string &period) const
{
    for (int m = 0; m < (int)data_.classrooms.periods.size(); ++m)
    {
        if (data_.classrooms.periods[m] == period)
            return m;
    }
    return -1;
}
