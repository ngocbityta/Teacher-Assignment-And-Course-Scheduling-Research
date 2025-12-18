// phase3.cpp
#include "phase3.h"
#include <vector>
#include <algorithm>
#include <random>
#include <chrono>
#include <cmath>
#include <iostream>
#include <deque>
#include <numeric>
#include <unordered_set>
#include <unordered_map>
#include <sstream>
#include <map>

using namespace std;

namespace
{
    mt19937 rng(chrono::steady_clock::now().time_since_epoch().count());

    // Build string key for slot
    static inline string slot_key(const string &day, const string &period)
    {
        return day + "|" + period;
    }

    // Index structure to allow O(1) feasibility checks (per candidate)
    struct SolIndex
    {
        // slot -> count
        unordered_map<string, int> slot_count;

        // teacher -> set of slot_keys (busy)
        unordered_map<string, unordered_set<string>> teacher_busy;

        // course -> slot_key -> section_id (to check same-course conflict)
        unordered_map<string, unordered_map<string, string>> course_slot_section;

        // course -> set of teachers teaching it (for min/max teacher bounds)
        unordered_map<string, unordered_set<string>> course_teachers;

        // classroom_id -> set of slot_keys (busy classrooms)
        unordered_map<string, unordered_set<string>> classroom_busy;
    };

    static SolIndex build_index(const OptimalSolution &sol, const ProblemData &data)
    {
        SolIndex idx;
        for (const auto &a : sol.assignments)
        {
            // Index all periods in the block, not just the start period
            int required_periods = get_required_periods(data, a.course_id, a.section_id);
            int start_period_idx = find_period_index(data.classrooms.periods, a.period);
            
            if (start_period_idx >= 0)
            {
                for (int t = 0; t < required_periods && start_period_idx + t < (int)data.classrooms.periods.size(); ++t)
                {
                    string period = data.classrooms.periods[start_period_idx + t];
                    string sk = slot_key(a.day, period);
                    idx.slot_count[sk]++;
                    idx.teacher_busy[a.teacher_id].insert(sk);
                    idx.course_slot_section[a.course_id][sk] = a.section_id;
                    idx.course_teachers[a.course_id].insert(a.teacher_id);
                    if (!a.classroom_id.empty())
                        idx.classroom_busy[a.classroom_id].insert(sk);
                }
            }
        }
        return idx;
    }

    static int get_required_periods(const ProblemData &data, const string &course_id, const string &section_id)
    {
        for (const auto &c : data.courses)
        {
            if (c.id == course_id)
            {
                for (const auto &s : c.sections)
                    if (s.id == section_id)
                        return s.required_periods;
            }
        }
        return 1;
    }

    // Helper function to find period index in periods vector
    static int find_period_index(const vector<string> &periods, const string &period)
    {
        for (int m = 0; m < (int)periods.size(); ++m)
        {
            if (periods[m] == period)
                return m;
        }
        return -1;
    }

    static int get_required_seats(const ProblemData &data, const string &course_id, const string &section_id)
    {
        for (const auto &c : data.courses)
        {
            if (c.id == course_id)
            {
                for (const auto &s : c.sections)
                    if (s.id == section_id)
                        return s.required_seats;
            }
        }
        return 0;
    }

    // Tìm classroom phù hợp cho một assignment
    static string find_suitable_classroom(const ProblemData &data, 
                                          const string &course_id, 
                                          const string &section_id,
                                          const string &day,
                                          const string &period,
                                          const OptimalSolution &sol,
                                          const SolIndex &idx)
    {
        int required_seats = get_required_seats(data, course_id, section_id);
        string sk = slot_key(day, period);
        
        if (data.classrooms.classrooms.empty())
            return "";
        
        // Tìm classroom có đủ capacity và chưa được sử dụng trong slot này
        for (const auto &room : data.classrooms.classrooms)
        {
            if (room.capacity >= required_seats)
            {
                auto it_cb = idx.classroom_busy.find(room.id);
                if (it_cb == idx.classroom_busy.end() || it_cb->second.find(sk) == it_cb->second.end())
                    return room.id; // Tìm thấy classroom phù hợp
            }
        }
        return "";
    }

    static string assignment_sig(const OptimalSolution::Assignment &a)
    {
        ostringstream oss;
        oss << a.teacher_id << "|" << a.course_id << "|" << a.section_id << "|" << a.day << "|" << a.period;
        return oss.str();
    }

    static string pair_sig(const OptimalSolution::Assignment &a, const OptimalSolution::Assignment &b)
    {
        string s1 = assignment_sig(a), s2 = assignment_sig(b);
        if (s1 < s2)
            return s1 + "<=>" + s2;
        return s2 + "<=>" + s1;
    }

    // ---------- Feasibility using index ----------
    // Check course teacher bounds using idx
    static bool check_course_teacher_bounds(const SolIndex &idx, const ProblemData &data)
    {
        for (const auto &c : data.courses)
        {
            auto it = idx.course_teachers.find(c.id);
            int cnt = (it == idx.course_teachers.end()) ? 0 : (int)it->second.size();
            if (cnt < c.min_teachers || cnt > c.max_teachers)
                return false;
        }
        return true;
    }

    // IsFeasibleBlock now uses the prebuilt index for O(1) lookups
    static bool IsFeasibleBlock(const string &teacher_id,
                                const string &course_id,
                                const string &section_id,
                                const string &day,
                                const string &start_period,
                                const string &classroom_id,
                                const OptimalSolution &sol,
                                const SolIndex &idx,
                                const ProblemData &data)
    {
        // teacher exists and eligible
        auto it_teacher = find_if(data.teachers.begin(), data.teachers.end(),
                                  [&](const Teacher &t)
                                  { return t.id == teacher_id; });
        if (it_teacher == data.teachers.end())
            return false;
        if (find(it_teacher->eligible_courses.begin(), it_teacher->eligible_courses.end(), course_id) == it_teacher->eligible_courses.end())
            return false;

        int r = get_required_periods(data, course_id, section_id);

        // find start idx in periods
        int start_idx = find_period_index(data.classrooms.periods, start_period);
        if (start_idx < 0 || start_idx + r - 1 >= (int)data.classrooms.periods.size())
            return false;

        for (int t = 0; t < r; ++t)
        {
            string period = data.classrooms.periods[start_idx + t];
            string sk = slot_key(day, period);

            // --- teacher slot conflict (O(1)) ---
            auto it_tb = idx.teacher_busy.find(teacher_id);
            if (it_tb != idx.teacher_busy.end() && it_tb->second.find(sk) != it_tb->second.end())
                return false;

            // --- classroom capacity check ---
            int required_seats = get_required_seats(data, course_id, section_id);
            if (!classroom_id.empty() && !data.classrooms.classrooms.empty())
            {
                // Tìm classroom và kiểm tra capacity
                auto it_classroom = find_if(data.classrooms.classrooms.begin(), data.classrooms.classrooms.end(),
                                           [&](const Classroom &c) { return c.id == classroom_id; });
                if (it_classroom == data.classrooms.classrooms.end())
                    return false; // classroom không tồn tại
                if (it_classroom->capacity < required_seats)
                    return false; // classroom không đủ chỗ ngồi
                
                // Kiểm tra xem classroom đã được sử dụng trong time slot này chưa
                auto it_cb = idx.classroom_busy.find(classroom_id);
                if (it_cb != idx.classroom_busy.end() && it_cb->second.find(sk) != it_cb->second.end())
                    return false; // classroom đã được sử dụng trong slot này
            }
            else
            {
                // Fallback: sử dụng Clm nếu không có classrooms được định nghĩa
                auto day_it = data.classrooms.Clm.find(day);
                if (day_it != data.classrooms.Clm.end())
                {
                    auto period_it = day_it->second.find(period);
                    if (period_it != day_it->second.end())
                    {
                        int cap = period_it->second;
                        int cnt = 0;
                        auto it_sc = idx.slot_count.find(sk);
                        if (it_sc != idx.slot_count.end())
                            cnt = it_sc->second;
                        if (cnt >= cap)
                            return false;
                    }
                }
            }

            // --- same-course conflict: check idx.course_slot_section ---
            auto it_cs = idx.course_slot_section.find(course_id);
            if (it_cs != idx.course_slot_section.end())
            {
                auto it_slot = it_cs->second.find(sk);
                if (it_slot != it_cs->second.end() && it_slot->second != section_id)
                    return false;
            }
        }
        return true;
    }

    // Convenience wrapper for single-period feasibility (keeps compatibility)
    static bool IsFeasible(const string &teacher_id,
                           const string &course_id,
                           const string &section_id,
                           const string &day,
                           const string &period,
                           const string &classroom_id,
                           const OptimalSolution &sol,
                           const SolIndex &idx,
                           const ProblemData &data)
    {
        return IsFeasibleBlock(teacher_id, course_id, section_id, day, period, classroom_id, sol, idx, data);
    }

    // ---------- Objective Evaluation ----------
    static double compute_stddev(const vector<int> &vals)
    {
        if (vals.empty())
            return 0.0;
        double mean = accumulate(vals.begin(), vals.end(), 0.0) / vals.size();
        double s = 0.0;
        for (double v : vals)
            s += (v - mean) * (v - mean);
        return sqrt(s / vals.size());
    }

    // Build teacher lookup map for O(1) access
    static unordered_map<string, const Teacher*> build_teacher_map(const ProblemData &data)
    {
        unordered_map<string, const Teacher*> teacher_map;
        for (const auto &t : data.teachers)
            teacher_map[t.id] = &t;
        return teacher_map;
    }

    // Build time preference lookup map: teacher_id -> (day, period) -> score
    static unordered_map<string, unordered_map<string, int>> build_time_pref_map(const ProblemData &data)
    {
        unordered_map<string, unordered_map<string, int>> time_pref_map;
        for (const auto &t : data.teachers)
        {
            for (const auto &tp : t.time_pref)
            {
                string key = tp.day + "|" + tp.period;
                time_pref_map[t.id][key] = tp.score;
            }
        }
        return time_pref_map;
    }

    static int Evaluate(const OptimalSolution &sol, 
                        const ProblemData &data, 
                        const unordered_map<string, const Teacher*> &teacher_map,
                        const unordered_map<string, unordered_map<string, int>> &time_pref_map,
                        int history_count = 0)
    {
        int score = 0;
        for (const auto &a : sol.assignments)
        {
            auto it_t = teacher_map.find(a.teacher_id);
            if (it_t != teacher_map.end())
            {
                const Teacher* teacher = it_t->second;
                
                auto pc_it = teacher->course_pref.find(a.course_id);
                if (pc_it != teacher->course_pref.end())
                    score += pc_it->second;
                
                int required_periods = get_required_periods(data, a.course_id, a.section_id);
                int start_period_idx = find_period_index(data.classrooms.periods, a.period);
                
                if (start_period_idx >= 0)
                {
                    for (int t = 0; t < required_periods && start_period_idx + t < (int)data.classrooms.periods.size(); ++t)
                    {
                        string period = data.classrooms.periods[start_period_idx + t];
                        string key = a.day + "|" + period;
                        auto tp_it = time_pref_map.find(a.teacher_id);
                        if (tp_it != time_pref_map.end())
                        {
                            auto score_it = tp_it->second.find(key);
                            if (score_it != tp_it->second.end())
                                score += score_it->second;
                        }
                    }
                }
            }
        }
        if (history_count > 3)
            score -= (history_count - 3);
        return score;
    }

    // ---------- Move operators (free functions) ----------
    // Each returns pair<changed, signature-string>
    // Note: each move builds index(cand) once and uses it to test feasibility & course bounds

    static pair<bool, string> move_single_change(OptimalSolution &sol, const ProblemData &data)
    {
        if (sol.assignments.empty())
            return {false, ""};
        uniform_int_distribution<int> dist(0, (int)sol.assignments.size() - 1);
        int idx_assign = dist(rng);
        auto &a = sol.assignments[idx_assign];

        // find course object
        auto it_course = find_if(data.courses.begin(), data.courses.end(),
                                 [&](const Course &c)
                                 { return c.id == a.course_id; });
        if (it_course == data.courses.end() || it_course->Ij.empty())
            return {false, ""};
        const auto &course = *it_course;

        // try teacher change
        uniform_int_distribution<int> tdist(0, (int)course.Ij.size() - 1);
        string new_teacher = course.Ij[tdist(rng)];
        if (new_teacher != a.teacher_id)
        {
            OptimalSolution cand = sol;
            cand.assignments[idx_assign].teacher_id = new_teacher;
            SolIndex idx = build_index(cand, data);
            if (!check_course_teacher_bounds(idx, data))
                ; // reject due to teacher bounds
            else if (IsFeasible(new_teacher, a.course_id, a.section_id, a.day, a.period, a.classroom_id, cand, idx, data))
            {
                OptimalSolution::Assignment old = a;
                a.teacher_id = new_teacher;
                return {true, pair_sig(old, a)};
            }
        }

        // try relocate (few attempts)
        uniform_int_distribution<int> ldist(0, (int)data.classrooms.days.size() - 1);
        uniform_int_distribution<int> pdist(0, (int)data.classrooms.periods.size() - 1);
        for (int t = 0; t < 6; ++t)
        {
            string new_day = data.classrooms.days[ldist(rng)];
            string new_period = data.classrooms.periods[pdist(rng)];
            if (new_day == a.day && new_period == a.period)
                continue;
            OptimalSolution cand = sol;
            cand.assignments[idx_assign].day = new_day;
            cand.assignments[idx_assign].period = new_period;
            SolIndex idx_temp = build_index(cand, data);
            // Tìm classroom phù hợp cho time slot mới
            string new_classroom_id = find_suitable_classroom(data, a.course_id, a.section_id, new_day, new_period, cand, idx_temp);
            if (new_classroom_id.empty())
                continue;
            cand.assignments[idx_assign].classroom_id = new_classroom_id;
            SolIndex idx = build_index(cand, data);
            if (!check_course_teacher_bounds(idx, data))
                continue;
            if (IsFeasible(a.teacher_id, a.course_id, a.section_id, new_day, new_period, new_classroom_id, cand, idx, data))
            {
                OptimalSolution::Assignment old = a;
                a.day = new_day;
                a.period = new_period;
                a.classroom_id = new_classroom_id;
                return {true, pair_sig(old, a)};
            }
        }
        return {false, ""};
    }

    static pair<bool, string> move_teacher_swap(OptimalSolution &sol, const ProblemData &data)
    {
        if (sol.assignments.size() < 2)
            return {false, ""};
        uniform_int_distribution<int> d(0, (int)sol.assignments.size() - 1);
        int i = d(rng), j = d(rng);
        if (i == j)
            return {false, ""};
        auto &A = sol.assignments[i];
        auto &B = sol.assignments[j];
        if (A.teacher_id == B.teacher_id)
            return {false, ""};

        OptimalSolution cand = sol;
        swap(cand.assignments[i].teacher_id, cand.assignments[j].teacher_id);
        SolIndex idx = build_index(cand, data);
        if (!check_course_teacher_bounds(idx, data))
            return {false, ""};

        if (IsFeasible(cand.assignments[i].teacher_id, A.course_id, A.section_id, A.day, A.period, A.classroom_id, cand, idx, data) &&
            IsFeasible(cand.assignments[j].teacher_id, B.course_id, B.section_id, B.day, B.period, B.classroom_id, cand, idx, data))
        {
            OptimalSolution::Assignment oldA = A, oldB = B;
            swap(A.teacher_id, B.teacher_id);
            return {true, pair_sig(oldA, oldB)};
        }
        return {false, ""};
    }

    static pair<bool, string> move_pair_swap(OptimalSolution &sol, const ProblemData &data)
    {
        if (sol.assignments.size() < 2)
            return {false, ""};
        uniform_int_distribution<int> d(0, (int)sol.assignments.size() - 1);
        int i = d(rng), j = d(rng);
        if (i == j)
            return {false, ""};
        auto a = sol.assignments[i];
        auto b = sol.assignments[j];

        // simulate swap entire (teacher+slot)
        OptimalSolution cand = sol;
        // place b into a's slot
        cand.assignments[i].teacher_id = b.teacher_id;
        cand.assignments[i].day = b.day;
        cand.assignments[i].period = b.period;
        cand.assignments[i].classroom_id = b.classroom_id;
        // place a into b's slot
        cand.assignments[j].teacher_id = a.teacher_id;
        cand.assignments[j].day = a.day;
        cand.assignments[j].period = a.period;
        cand.assignments[j].classroom_id = a.classroom_id;

        SolIndex idx = build_index(cand, data);
        if (!check_course_teacher_bounds(idx, data))
            return {false, ""};

        if (IsFeasible(cand.assignments[i].teacher_id, a.course_id, a.section_id, cand.assignments[i].day, cand.assignments[i].period, cand.assignments[i].classroom_id, cand, idx, data) &&
            IsFeasible(cand.assignments[j].teacher_id, b.course_id, b.section_id, cand.assignments[j].day, cand.assignments[j].period, cand.assignments[j].classroom_id, cand, idx, data))
        {
            swap(sol.assignments[i].teacher_id, sol.assignments[j].teacher_id);
            swap(sol.assignments[i].day, sol.assignments[j].day);
            swap(sol.assignments[i].period, sol.assignments[j].period);
            swap(sol.assignments[i].classroom_id, sol.assignments[j].classroom_id);
            return {true, pair_sig(a, b)};
        }
        return {false, ""};
    }

    static pair<bool, string> move_block_relocate(OptimalSolution &sol, const ProblemData &data)
    {
        if (sol.assignments.empty())
            return {false, ""};
        uniform_int_distribution<int> d(0, (int)sol.assignments.size() - 1);
        int idx_assign = d(rng);
        auto &a = sol.assignments[idx_assign];
        int attempts = 6;
        uniform_int_distribution<int> ldist(0, (int)data.classrooms.days.size() - 1);
        uniform_int_distribution<int> pdist(0, (int)data.classrooms.periods.size() - 1);
        for (int t = 0; t < attempts; ++t)
        {
            string nd = data.classrooms.days[ldist(rng)];
            string np = data.classrooms.periods[pdist(rng)];
            if (nd == a.day && np == a.period)
                continue;
            OptimalSolution cand = sol;
            cand.assignments[idx_assign].day = nd;
            cand.assignments[idx_assign].period = np;
            SolIndex idx_temp = build_index(cand, data);
            string new_classroom_id = find_suitable_classroom(data, a.course_id, a.section_id, nd, np, cand, idx_temp);
            if (new_classroom_id.empty())
                continue;
            cand.assignments[idx_assign].classroom_id = new_classroom_id;
            SolIndex idx = build_index(cand, data);
            if (!check_course_teacher_bounds(idx, data))
                continue;
            if (IsFeasibleBlock(a.teacher_id, a.course_id, a.section_id, nd, np, new_classroom_id, cand, idx, data))
            {
                OptimalSolution::Assignment old = a;
                a.day = nd;
                a.period = np;
                a.classroom_id = new_classroom_id;
                return {true, pair_sig(old, a)};
            }
        }
        return {false, ""};
    }

    static pair<bool, string> move_block_swap(OptimalSolution &sol, const ProblemData &data)
    {
        if (sol.assignments.size() < 2)
            return {false, ""};
        uniform_int_distribution<int> d(0, (int)sol.assignments.size() - 1);
        int i = d(rng), j = d(rng);
        if (i == j)
            return {false, ""};
        auto A = sol.assignments[i];
        auto B = sol.assignments[j];

        OptimalSolution cand = sol;
        cand.assignments[i].day = B.day;
        cand.assignments[i].period = B.period;
        cand.assignments[i].teacher_id = B.teacher_id;
        cand.assignments[i].classroom_id = B.classroom_id;
        cand.assignments[j].day = A.day;
        cand.assignments[j].period = A.period;
        cand.assignments[j].teacher_id = A.teacher_id;
        cand.assignments[j].classroom_id = A.classroom_id;

        SolIndex idx = build_index(cand, data);
        if (!check_course_teacher_bounds(idx, data))
            return {false, ""};

        if (IsFeasibleBlock(cand.assignments[i].teacher_id, A.course_id, A.section_id, cand.assignments[i].day, cand.assignments[i].period, cand.assignments[i].classroom_id, cand, idx, data) &&
            IsFeasibleBlock(cand.assignments[j].teacher_id, B.course_id, B.section_id, cand.assignments[j].day, cand.assignments[j].period, cand.assignments[j].classroom_id, cand, idx, data))
        {
            swap(sol.assignments[i].day, sol.assignments[j].day);
            swap(sol.assignments[i].period, sol.assignments[j].period);
            swap(sol.assignments[i].teacher_id, sol.assignments[j].teacher_id);
            swap(sol.assignments[i].classroom_id, sol.assignments[j].classroom_id);
            return {true, pair_sig(A, B)};
        }
        return {false, ""};
    }

}

// ---------- Main Phase3 ----------
OptimalSolution find_optimal_solution(const ProblemData &data, const InitialSolution &initial)
{
    unordered_map<string, const Teacher*> teacher_map = build_teacher_map(data);
    unordered_map<string, unordered_map<string, int>> time_pref_map = build_time_pref_map(data);

    OptimalSolution current = initial;
    OptimalSolution temp = initial;
    OptimalSolution best = initial;

    current.objective_value = Evaluate(current, data, teacher_map, time_pref_map);
    temp.objective_value = current.objective_value;
    best.objective_value = current.objective_value;

    // Tabu + history
    deque<string> tabu_q;
    unordered_set<string> tabu_set;
    size_t tabu_tenure = 150; // tuneable
    unordered_map<string, int> history_count;

    // Neighborhoods ordered by increasing strength
    using MoveFn = pair<bool, string> (*)(OptimalSolution &, const ProblemData &);
    vector<MoveFn> neighborhoods = {
        &move_single_change,
        &move_teacher_swap,
        &move_pair_swap,
        &move_block_relocate,
        &move_block_swap};

    // SA/VNS params
    double T = 1.0;
    double alpha = 0.98;
    int max_iterations = 1200;
    int moves_per_nb = 40;
    int limit_no_improv = 300;

    deque<int> recent_objs;
    int numb_iter_no_improv = 0;

    for (int iter = 0; iter < max_iterations; ++iter)
    {
        bool any_improved = false;

        for (size_t nb = 0; nb < neighborhoods.size(); ++nb)
        {
            bool improved_in_nb = false;

            for (int mv = 0; mv < moves_per_nb; ++mv)
            {
                // Prepare candidate copy and try a move
                OptimalSolution cand = current;
                auto res = neighborhoods[nb](cand, data);
                if (!res.first)
                    continue;
                string sig = res.second;

                // Evaluate candidate once
                int cand_score = Evaluate(cand, data, teacher_map, time_pref_map, history_count[sig]);
                
                // check tabu (aspiration criterion: accept if better than best)
                if (!sig.empty() && tabu_set.find(sig) != tabu_set.end())
                {
                    if (cand_score <= best.objective_value)
                        continue; // reject unless aspiration
                }
                int delta = cand_score - temp.objective_value;

                bool accept = false;
                if (delta >= 0)
                    accept = true;
                else
                {
                    recent_objs.push_back(temp.objective_value);
                    if (recent_objs.size() > 100)
                        recent_objs.pop_front();
                    vector<int> tmp(recent_objs.begin(), recent_objs.end());
                    double sigma = compute_stddev(tmp);
                    double adaptive_T = 0.25 * T + 0.75 * (0.01 + sigma);
                    uniform_real_distribution<double> u(0.0, 1.0);
                    double prob = exp(delta / adaptive_T);
                    if (u(rng) < prob)
                        accept = true;
                }

                if (accept)
                {
                    current = cand;
                    temp = cand;
                    temp.objective_value = cand_score;

                    if (!sig.empty())
                    {
                        history_count[sig] += 1;
                        tabu_q.push_back(sig);
                        tabu_set.insert(sig);
                        if (tabu_q.size() > tabu_tenure)
                        {
                            string old = tabu_q.front();
                            tabu_q.pop_front();
                            tabu_set.erase(old);
                        }
                    }

                    if (cand_score > best.objective_value)
                    {
                        best = cand;
                        best.objective_value = cand_score;
                        numb_iter_no_improv = 0;
                    }
                    else
                        ++numb_iter_no_improv;

                    improved_in_nb = true;
                    any_improved = true;
                    // intensify: restart from smallest neighborhood
                    nb = 0;
                    break;
                }
            } // moves per neighborhood

            if (!improved_in_nb)
            {
                // go to next neighborhood (diversify)
                continue;
            }
        } // neighborhoods

        if (!any_improved)
        {
            // diversification: random shakes
            int shakes = 4;
            for (int s = 0; s < shakes; ++s)
            {
                OptimalSolution cand = current;
                pair<bool, string> res = move_block_relocate(cand, data);
                if (!res.first)
                    res = move_teacher_swap(cand, data);
                if (!res.first)
                    res = move_pair_swap(cand, data);
                if (!res.first)
                    continue;
                string sig = res.second;

                int cand_score = Evaluate(cand, data, teacher_map, time_pref_map, history_count[sig]);

                uniform_real_distribution<double> u(0.0, 1.0);
                recent_objs.push_back(temp.objective_value);
                if (recent_objs.size() > 100)
                    recent_objs.pop_front();
                vector<int> tmp(recent_objs.begin(), recent_objs.end());
                double sigma = compute_stddev(tmp);
                double adaptive_T = 0.25 * T + 0.75 * (0.01 + sigma);
                double prob = exp((cand_score - temp.objective_value) / adaptive_T);
                if (u(rng) < prob)
                {
                    current = cand;
                    temp = cand;
                    temp.objective_value = cand_score;
                    if (!sig.empty())
                    {
                        history_count[sig] += 1;
                        tabu_q.push_back(sig);
                        tabu_set.insert(sig);
                        if (tabu_q.size() > tabu_tenure)
                        {
                            string old = tabu_q.front();
                            tabu_q.pop_front();
                            tabu_set.erase(old);
                        }
                    }
                    if (cand_score > best.objective_value)
                    {
                        best = cand;
                        best.objective_value = cand_score;
                        numb_iter_no_improv = 0;
                    }
                    else
                        ++numb_iter_no_improv;
                    break;
                }
            }
        }

        // cooling & adapt alpha
        T *= alpha;
        if (any_improved)
            alpha = min(0.995, alpha + 0.0006);
        else
            alpha = max(0.92, alpha - 0.0009);

        // restart if stuck
        if (numb_iter_no_improv > limit_no_improv)
        {
            current = best;
            temp = best;
            numb_iter_no_improv = 0;
            shuffle(current.assignments.begin(), current.assignments.end(), rng);
            // small shake
            for (int k = 0; k < (int)current.assignments.size() / 12; ++k)
                move_single_change(current, data);
        }

        if (iter % 50 == 0)
        {
            cout << "[Phase3] Iter " << iter
                 << ", Best " << best.objective_value
                 << ", Temp " << T
                 << ", Tabu " << tabu_set.size()
                 << ", HistoryEntries " << history_count.size()
                 << "\n";
        }
    } // main loop

    cout << "[Phase3] Finished. Best objective: " << best.objective_value
         << ", Total assignments: " << best.assignments.size() << "\n";
    return best;
}
