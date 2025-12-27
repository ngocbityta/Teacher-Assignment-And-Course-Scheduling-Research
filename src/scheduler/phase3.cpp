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
#include <tuple>

using namespace std;

namespace
{
    mt19937 rng(chrono::steady_clock::now().time_since_epoch().count());

    // ---------- Move Structure (for tracking, not for tabu) ----------
    struct Move
    {
        enum Type { SINGLE_CHANGE, TEACHER_SWAP, PAIR_SWAP, BLOCK_RELOCATE, BLOCK_SWAP };
        Type type;
        vector<int> indices;  // assignment indices involved
        vector<string> old_values;  // old values (teacher_id, day, period, etc.)
        vector<string> new_values;  // new values
    };

    // ---------- Signature-Based Tabu List Structure ----------
    struct SignatureBasedTabu
    {
        deque<string> tabu_queue_tc;
        unordered_set<string> tabu_set_tc;
        deque<string> tabu_queue_ct;
        unordered_set<string> tabu_set_ct;
        size_t tenure_tc;
        size_t tenure_ct;

        SignatureBasedTabu(size_t tc_tenure = 50, size_t ct_tenure = 100)
            : tenure_tc(tc_tenure), tenure_ct(ct_tenure)
        {
        }

        bool is_tabu(const string &sig_tc, const string &sig_ct, double best_score, double cand_score) const
        {
            bool aspiration = (cand_score > best_score);

            if (!sig_tc.empty() && tabu_set_tc.find(sig_tc) != tabu_set_tc.end())
            {
                if (!aspiration)
                    return true;
            }

            if (!sig_ct.empty() && tabu_set_ct.find(sig_ct) != tabu_set_ct.end())
            {
                if (!aspiration)
                    return true;
            }

            return false;
        }

        void add_tabu(const string &sig_tc, const string &sig_ct)
        {
            if (!sig_tc.empty())
            {
                tabu_queue_tc.push_back(sig_tc);
                tabu_set_tc.insert(sig_tc);
                if (tabu_queue_tc.size() > tenure_tc)
                {
                    string old = tabu_queue_tc.front();
                    tabu_queue_tc.pop_front();
                    tabu_set_tc.erase(old);
                }
            }

            if (!sig_ct.empty())
            {
                tabu_queue_ct.push_back(sig_ct);
                tabu_set_ct.insert(sig_ct);
                if (tabu_queue_ct.size() > tenure_ct)
                {
                    string old = tabu_queue_ct.front();
                    tabu_queue_ct.pop_front();
                    tabu_set_ct.erase(old);
                }
            }
        }
    };
    
    static tuple<string, string> extract_move_signatures(const Move &move)
    {
        string sig_tc = "";
        string sig_ct = "";
        
        // old_values format: [teacher_id, day, period, classroom_id] for single
        // or [teacher_id1, day1, period1, classroom_id1, teacher_id2, ...] for pair
        if (move.old_values.size() >= 4 && move.new_values.size() >= 4)
        {
            string old_teacher = move.old_values[0];
            string new_teacher = move.new_values[0];
            string old_day = move.old_values[1];
            string old_period = move.old_values[2];
            string new_day = move.new_values[1];
            string new_period = move.new_values[2];
            
            if (old_teacher != new_teacher)
            {
                sig_tc = old_teacher + "<=>" + new_teacher;
            }
            
            if (old_day != new_day || old_period != new_period)
            {
                string old_slot = old_day + "|" + old_period;
                string new_slot = new_day + "|" + new_period;
                if (old_slot < new_slot)
                    sig_ct = old_slot + "<=>" + new_slot;
                else
                    sig_ct = new_slot + "<=>" + old_slot;
            }
        }
        
        if (move.old_values.size() >= 8 && move.new_values.size() >= 8)
        {
            string old_teacher1 = move.old_values[0];
            string old_teacher2 = move.old_values[4];
            string new_teacher1 = move.new_values[0];
            string new_teacher2 = move.new_values[4];
            
            if (old_teacher1 != new_teacher1 || old_teacher2 != new_teacher2)
            {
                string tc1 = old_teacher1 + "<=>" + new_teacher1;
                string tc2 = old_teacher2 + "<=>" + new_teacher2;
                if (tc1 < tc2)
                    sig_tc = tc1 + "|" + tc2;
                else
                    sig_tc = tc2 + "|" + tc1;
            }
            
            string old_slot1 = move.old_values[1] + "|" + move.old_values[2];
            string old_slot2 = move.old_values[5] + "|" + move.old_values[6];
            string new_slot1 = move.new_values[1] + "|" + move.new_values[2];
            string new_slot2 = move.new_values[5] + "|" + move.new_values[6];
            
            if (old_slot1 != new_slot1 || old_slot2 != new_slot2)
            {
                string ct1 = (old_slot1 < new_slot1) ? (old_slot1 + "<=>" + new_slot1) : (new_slot1 + "<=>" + old_slot1);
                string ct2 = (old_slot2 < new_slot2) ? (old_slot2 + "<=>" + new_slot2) : (new_slot2 + "<=>" + old_slot2);
                if (ct1 < ct2)
                    sig_ct = ct1 + "|" + ct2;
                else
                    sig_ct = ct2 + "|" + ct1;
            }
        }
        
        return make_tuple(sig_tc, sig_ct);
    }

    // ---------- Assignment Identity and Signature Helpers ----------
    static string get_assignment_id(const OptimalSolution::Assignment &a)
    {
        return a.course_id + "|" + a.section_id;
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

    static int find_period_index(const vector<string> &periods, const string &period)
    {
        for (int m = 0; m < (int)periods.size(); ++m)
        {
            if (periods[m] == period)
                return m;
        }
        return -1;
    }

    // ---------- Signature Generation Functions ----------
    static string generate_teacher_course_sig(const OptimalSolution::Assignment &a)
    {
        ostringstream oss;
        oss << a.teacher_id << "|" << a.course_id;
        return oss.str();
    }

    static string generate_teacher_course_sig_pair(const OptimalSolution::Assignment &a, 
                                                    const OptimalSolution::Assignment &b)
    {
        string sig1 = generate_teacher_course_sig(a);
        string sig2 = generate_teacher_course_sig(b);
        if (sig1 < sig2)
            return sig1 + "<=>" + sig2;
        return sig2 + "<=>" + sig1;
    }

    static string generate_course_timeslot_sig(const OptimalSolution::Assignment &a)
    {
        ostringstream oss;
        oss << a.course_id << "|" << a.day << "|" << a.period;
        return oss.str();
    }

    static string generate_course_timeslot_sig_pair(const OptimalSolution::Assignment &a,
                                                     const OptimalSolution::Assignment &b)
    {
        string sig1 = generate_course_timeslot_sig(a);
        string sig2 = generate_course_timeslot_sig(b);
        if (sig1 < sig2)
            return sig1 + "<=>" + sig2;
        return sig2 + "<=>" + sig1;
    }
    
    static tuple<string, string> extract_signatures_from_changes(
        const OptimalSolution &old_sol,
        const OptimalSolution &new_sol)
    {
        string sig_tc = "";
        string sig_ct = "";
        
        unordered_map<string, OptimalSolution::Assignment> old_map;
        for (const auto &a : old_sol.assignments)
        {
            string assignment_id = get_assignment_id(a);
            old_map[assignment_id] = a;
        }
        
        vector<string> tc_changes;
        vector<string> ct_changes;
        
        for (const auto &new_a : new_sol.assignments)
        {
            string assignment_id = get_assignment_id(new_a);
            auto it = old_map.find(assignment_id);
            if (it != old_map.end())
            {
                const auto &old_a = it->second;
                
                // Check teacher change
                if (old_a.teacher_id != new_a.teacher_id)
                {
                    string tc_old = generate_teacher_course_sig(old_a);
                    string tc_new = generate_teacher_course_sig(new_a);
                    if (tc_old != tc_new)
                    {
                        if (tc_old < tc_new)
                            tc_changes.push_back(tc_old + "<=>" + tc_new);
                        else
                            tc_changes.push_back(tc_new + "<=>" + tc_old);
                    }
                }
                
                // Check timeslot change
                if (old_a.day != new_a.day || old_a.period != new_a.period)
                {
                    string ct_old = generate_course_timeslot_sig(old_a);
                    string ct_new = generate_course_timeslot_sig(new_a);
                    if (ct_old != ct_new)
                    {
                        if (ct_old < ct_new)
                            ct_changes.push_back(ct_old + "<=>" + ct_new);
                        else
                            ct_changes.push_back(ct_new + "<=>" + ct_old);
                    }
                }
            }
        }
        
        if (!tc_changes.empty())
        {
            sort(tc_changes.begin(), tc_changes.end());
            ostringstream oss;
            for (size_t i = 0; i < tc_changes.size(); ++i)
            {
                if (i > 0) oss << "|";
                oss << tc_changes[i];
            }
            sig_tc = oss.str();
        }
        
        if (!ct_changes.empty())
        {
            sort(ct_changes.begin(), ct_changes.end());
            ostringstream oss;
            for (size_t i = 0; i < ct_changes.size(); ++i)
            {
                if (i > 0) oss << "|";
                oss << ct_changes[i];
            }
            sig_ct = oss.str();
        }
        
        return make_tuple(sig_tc, sig_ct);
    }

    static string generate_full_sig(const OptimalSolution::Assignment &a)
    {
        return assignment_sig(a);
    }

    static string generate_full_sig_pair(const OptimalSolution::Assignment &a,
                                          const OptimalSolution::Assignment &b)
    {
        return pair_sig(a, b);
    }

    static tuple<string, string, string> generate_all_signatures(
        const OptimalSolution::Assignment &old_assignment,
        const OptimalSolution::Assignment &new_assignment)
    {
        string sig_tc_old = generate_teacher_course_sig(old_assignment);
        string sig_tc_new = generate_teacher_course_sig(new_assignment);
        string sig_tc = (sig_tc_old != sig_tc_new) ? (sig_tc_old < sig_tc_new ? sig_tc_old + "<=>" + sig_tc_new : sig_tc_new + "<=>" + sig_tc_old) : "";
        
        string sig_ct_old = generate_course_timeslot_sig(old_assignment);
        string sig_ct_new = generate_course_timeslot_sig(new_assignment);
        string sig_ct = (sig_ct_old != sig_ct_new) ? (sig_ct_old < sig_ct_new ? sig_ct_old + "<=>" + sig_ct_new : sig_ct_new + "<=>" + sig_ct_old) : "";
        
        string sig_full_old = generate_full_sig(old_assignment);
        string sig_full_new = generate_full_sig(new_assignment);
        string sig_full = (sig_full_old != sig_full_new) ? (sig_full_old < sig_full_new ? sig_full_old + "<=>" + sig_full_new : sig_full_new + "<=>" + sig_full_old) : "";
        
        return make_tuple(sig_tc, sig_ct, sig_full);
    }

    static tuple<string, string, string> generate_all_signatures_pair(
        const OptimalSolution::Assignment &a,
        const OptimalSolution::Assignment &b)
    {
        string sig_tc = generate_teacher_course_sig_pair(a, b);
        string sig_ct = generate_course_timeslot_sig_pair(a, b);
        string sig_full = generate_full_sig_pair(a, b);
        return make_tuple(sig_tc, sig_ct, sig_full);
    }

    static vector<pair<OptimalSolution::Assignment, OptimalSolution::Assignment>>
    find_changed_assignments(const OptimalSolution &old_sol, const OptimalSolution &new_sol)
    {
        vector<pair<OptimalSolution::Assignment, OptimalSolution::Assignment>> changes;
        
        unordered_map<string, OptimalSolution::Assignment> old_map;
        for (const auto &a : old_sol.assignments)
        {
            string assignment_id = get_assignment_id(a);
            old_map[assignment_id] = a;
        }

        for (const auto &new_a : new_sol.assignments)
        {
            string assignment_id = get_assignment_id(new_a);
            auto it = old_map.find(assignment_id);
            if (it != old_map.end())
            {
                const auto &old_a = it->second;
                if (old_a.teacher_id != new_a.teacher_id || 
                    old_a.day != new_a.day ||
                    old_a.period != new_a.period ||
                    old_a.classroom_id != new_a.classroom_id)
                {
                    changes.push_back({old_a, new_a});
                }
            }
            else
            {
                for (const auto &old_a : old_sol.assignments)
                {
                    if (old_a.course_id == new_a.course_id && old_a.section_id == new_a.section_id)
                    {
                        changes.push_back({old_a, new_a});
                        break;
                    }
                }
            }
        }

        return changes;
    }

    static inline string slot_key(const string &day, const string &period)
    {
        return day + "|" + period;
    }

    struct SolIndex
    {
        unordered_map<string, int> slot_count;
        unordered_map<string, unordered_set<string>> teacher_busy;
        unordered_map<string, unordered_map<string, string>> course_slot_section;
        unordered_map<string, unordered_set<string>> course_teachers;
        unordered_map<string, unordered_set<string>> classroom_busy;
    };

    static SolIndex build_index(const OptimalSolution &sol, const ProblemData &data)
    {
        SolIndex idx;
        for (const auto &a : sol.assignments)
        {
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
        
        for (const auto &room : data.classrooms.classrooms)
        {
            if (room.capacity >= required_seats)
            {
                auto it_cb = idx.classroom_busy.find(room.id);
                if (it_cb == idx.classroom_busy.end() || it_cb->second.find(sk) == it_cb->second.end())
                    return room.id;
            }
        }
        return "";
    }


    // ---------- Feasibility using index ----------
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
        auto it_teacher = find_if(data.teachers.begin(), data.teachers.end(),
                                  [&](const Teacher &t)
                                  { return t.id == teacher_id; });
        if (it_teacher == data.teachers.end())
            return false;
        if (find(it_teacher->eligible_courses.begin(), it_teacher->eligible_courses.end(), course_id) == it_teacher->eligible_courses.end())
            return false;

        int r = get_required_periods(data, course_id, section_id);

        int start_idx = find_period_index(data.classrooms.periods, start_period);
        if (start_idx < 0 || start_idx + r - 1 >= (int)data.classrooms.periods.size())
            return false;

        for (int t = 0; t < r; ++t)
        {
            string period = data.classrooms.periods[start_idx + t];
            string sk = slot_key(day, period);

            auto it_tb = idx.teacher_busy.find(teacher_id);
            if (it_tb != idx.teacher_busy.end() && it_tb->second.find(sk) != it_tb->second.end())
                return false;

            int required_seats = get_required_seats(data, course_id, section_id);
            if (!classroom_id.empty() && !data.classrooms.classrooms.empty())
            {
                auto it_classroom = find_if(data.classrooms.classrooms.begin(), data.classrooms.classrooms.end(),
                                           [&](const Classroom &c) { return c.id == classroom_id; });
                if (it_classroom == data.classrooms.classrooms.end())
                    return false;
                if (it_classroom->capacity < required_seats)
                    return false;
                
                auto it_cb = idx.classroom_busy.find(classroom_id);
                if (it_cb != idx.classroom_busy.end() && it_cb->second.find(sk) != it_cb->second.end())
                    return false;
            }
            else
            {
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

    static double compute_variance(const vector<double> &vals)
    {
        if (vals.empty())
            return 0.0;
        double mean = accumulate(vals.begin(), vals.end(), 0.0) / vals.size();
        double s = 0.0;
        for (double v : vals)
            s += (v - mean) * (v - mean);
        return s / vals.size();
    }

    static double compute_acceptance_probability(
        double delta,
        double current_score,
        const deque<double> &recent_objs,
        double base_T)
    {
        if (delta >= 0.0)
            return 1.0;

        double adaptive_T = base_T;
        
        if (!recent_objs.empty() && recent_objs.size() >= 2)
        {
            vector<double> tmp(recent_objs.begin(), recent_objs.end());
            double variance = compute_variance(tmp);
            double stddev = sqrt(variance);
            if (stddev > 1e-6)
            {
                // Use a fraction of stddev as temperature scale
                adaptive_T = max(0.1 * stddev, base_T * 0.5);
        }
        else
        {
                adaptive_T = base_T;
            }
        }

        adaptive_T = max(adaptive_T, 0.01);
        
        double exponent = delta / adaptive_T;
        
        const double MAX_EXPONENT = 700.0;
        const double MIN_EXPONENT = -700.0;
        
        if (exponent > MAX_EXPONENT)
        {
            return 1.0;
        }
        else if (exponent < MIN_EXPONENT)
        {
            return 0.0;
        }
        else
        {
            double prob = exp(exponent);
            return max(0.0, min(1.0, prob));
        }
    }

    static double compute_workload_balance_penalty(const OptimalSolution &sol, const ProblemData &data)
    {
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
        
        if (teacher_workloads.empty())
            return 0.0;
        
        vector<int> workloads;
        workloads.reserve(teacher_workloads.size());
        for (const auto &tw : teacher_workloads)
        {
            workloads.push_back(tw.second);
        }
        
        double stddev = compute_stddev(workloads);
        
        return stddev;
    }

    static double compute_compactness_penalty(const OptimalSolution &sol, const ProblemData &data)
    {
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
        
        double total_penalty = 0.0;
        
        for (const auto &teacher_entry : teacher_day_periods)
        {
            for (const auto &day_entry : teacher_entry.second)
            {
                vector<int> periods = day_entry.second;
                if (periods.empty())
                    continue;
                
                sort(periods.begin(), periods.end());
                periods.erase(unique(periods.begin(), periods.end()), periods.end());
                
                for (size_t i = 0; i < periods.size() - 1; ++i)
                {
                    int gap = periods[i + 1] - periods[i];
                    if (gap > 1)
                    {
                        total_penalty += (gap - 1);
                    }
                }
            }
        }
        
        return total_penalty;
    }

    static unordered_map<string, const Teacher*> build_teacher_map(const ProblemData &data)
    {
        unordered_map<string, const Teacher*> teacher_map;
        for (const auto &t : data.teachers)
            teacher_map[t.id] = &t;
        return teacher_map;
    }

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

    static double Evaluate(const OptimalSolution &sol, 
                        const ProblemData &data, 
                        const unordered_map<string, const Teacher*> &teacher_map,
                        const unordered_map<string, unordered_map<string, int>> &time_pref_map,
                        const OptimalSolution &initial_sol)
    {
        const double w_course_pref = 1.0;
        const double w_time_pref = 1.0;
        const double w_workload_balance = 5.0;
        const double w_compactness = 3.0;
        const double w_stability = 3.0;
        
        double score = 0.0;
        
        double course_pref_score = 0.0;
        for (const auto &a : sol.assignments)
        {
            auto it_t = teacher_map.find(a.teacher_id);
            if (it_t != teacher_map.end())
            {
                const Teacher* teacher = it_t->second;
                auto pc_it = teacher->course_pref.find(a.course_id);
                if (pc_it != teacher->course_pref.end())
                    course_pref_score += pc_it->second;
            }
        }
        score += w_course_pref * course_pref_score;
        
        double time_pref_score = 0.0;
        
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
                    
                    auto tp_it = time_pref_map.find(a.teacher_id);
                    if (tp_it != time_pref_map.end())
                    {
                        auto score_it = tp_it->second.find(key);
                        if (score_it != tp_it->second.end())
                            time_pref_score += score_it->second;
                    }
                }
            }
        }
        score += w_time_pref * time_pref_score;
        
        double workload_penalty = compute_workload_balance_penalty(sol, data);
        score -= w_workload_balance * workload_penalty;
        
        double compactness_penalty = compute_compactness_penalty(sol, data);
        score -= w_compactness * compactness_penalty;
        
        double stability_penalty = 0.0;
        unordered_map<string, OptimalSolution::Assignment> initial_map;
        for (const auto &a : initial_sol.assignments)
        {
            string assignment_id = get_assignment_id(a);
            initial_map[assignment_id] = a;
        }
        
        for (const auto &a : sol.assignments)
        {
            string assignment_id = get_assignment_id(a);
            auto it = initial_map.find(assignment_id);
            if (it != initial_map.end())
            {
                const auto &initial_a = it->second;
                if (a.teacher_id != initial_a.teacher_id)
                    stability_penalty += 1.0;
                string current_timeslot = a.day + "|" + a.period;
                string initial_timeslot = initial_a.day + "|" + initial_a.period;
                if (current_timeslot != initial_timeslot)
                    stability_penalty += 1.0;
            }
        }
        score -= w_stability * stability_penalty;
        
        return score;
    }

    static tuple<bool, Move> move_single_change(
        OptimalSolution &sol_out, 
        const OptimalSolution &sol_in, 
        const ProblemData &data)
    {
        Move move;
        move.type = Move::SINGLE_CHANGE;
        
        if (sol_in.assignments.empty())
            return make_tuple(false, move);
        
        uniform_int_distribution<int> dist(0, (int)sol_in.assignments.size() - 1);
        int idx_assign = dist(rng);
        const auto &a = sol_in.assignments[idx_assign];
        move.indices.push_back(idx_assign);

        auto it_course = find_if(data.courses.begin(), data.courses.end(),
                                 [&](const Course &c)
                                 { return c.id == a.course_id; });
        if (it_course == data.courses.end() || it_course->Ij.empty())
            return make_tuple(false, move);
        const auto &course = *it_course;

        uniform_int_distribution<int> tdist(0, (int)course.Ij.size() - 1);
        string new_teacher = course.Ij[tdist(rng)];
        if (new_teacher != a.teacher_id)
        {
            OptimalSolution cand = sol_in;
            cand.assignments[idx_assign].teacher_id = new_teacher;
            SolIndex idx = build_index(cand, data);
            if (check_course_teacher_bounds(idx, data) && 
                IsFeasible(new_teacher, a.course_id, a.section_id, a.day, a.period, a.classroom_id, cand, idx, data))
            {
                sol_out = cand;
                move.old_values = {a.teacher_id, a.day, a.period, a.classroom_id};
                move.new_values = {new_teacher, a.day, a.period, a.classroom_id};
                return make_tuple(true, move);
            }
        }

        uniform_int_distribution<int> ldist(0, (int)data.classrooms.days.size() - 1);
        uniform_int_distribution<int> pdist(0, (int)data.classrooms.periods.size() - 1);
        for (int t = 0; t < 6; ++t)
        {
            string new_day = data.classrooms.days[ldist(rng)];
            string new_period = data.classrooms.periods[pdist(rng)];
            if (new_day == a.day && new_period == a.period)
                continue;
            OptimalSolution cand = sol_in;
            cand.assignments[idx_assign].day = new_day;
            cand.assignments[idx_assign].period = new_period;
            SolIndex idx_temp = build_index(cand, data);
            string new_classroom_id = find_suitable_classroom(data, a.course_id, a.section_id, new_day, new_period, cand, idx_temp);
            if (new_classroom_id.empty())
                continue;
            cand.assignments[idx_assign].classroom_id = new_classroom_id;
            SolIndex idx = build_index(cand, data);
            if (!check_course_teacher_bounds(idx, data))
                continue;
            if (IsFeasible(a.teacher_id, a.course_id, a.section_id, new_day, new_period, new_classroom_id, cand, idx, data))
            {
                sol_out = cand;
                move.old_values = {a.teacher_id, a.day, a.period, a.classroom_id};
                move.new_values = {a.teacher_id, new_day, new_period, new_classroom_id};
                return make_tuple(true, move);
            }
        }
        return make_tuple(false, move);
    }

    static tuple<bool, Move> move_teacher_swap(
        OptimalSolution &sol_out,
        const OptimalSolution &sol_in,
        const ProblemData &data)
    {
        Move move;
        move.type = Move::TEACHER_SWAP;
        
        if (sol_in.assignments.size() < 2)
            return make_tuple(false, move);
        
        uniform_int_distribution<int> d(0, (int)sol_in.assignments.size() - 1);
        int i = d(rng), j = d(rng);
        if (i == j)
            return make_tuple(false, move);
        
        const auto &A = sol_in.assignments[i];
        const auto &B = sol_in.assignments[j];
        if (A.teacher_id == B.teacher_id)
            return make_tuple(false, move);

        OptimalSolution cand = sol_in;
        swap(cand.assignments[i].teacher_id, cand.assignments[j].teacher_id);
        SolIndex idx = build_index(cand, data);
        if (!check_course_teacher_bounds(idx, data))
            return make_tuple(false, move);

        if (IsFeasible(cand.assignments[i].teacher_id, A.course_id, A.section_id, A.day, A.period, A.classroom_id, cand, idx, data) &&
            IsFeasible(cand.assignments[j].teacher_id, B.course_id, B.section_id, B.day, B.period, B.classroom_id, cand, idx, data))
        {
            sol_out = cand;
            move.indices = {i, j};
            move.old_values = {A.teacher_id, B.teacher_id};
            move.new_values = {B.teacher_id, A.teacher_id};
            return make_tuple(true, move);
        }
        return make_tuple(false, move);
    }

    static tuple<bool, Move> move_pair_swap(
        OptimalSolution &sol_out,
        const OptimalSolution &sol_in,
        const ProblemData &data)
    {
        Move move;
        move.type = Move::PAIR_SWAP;
        
        if (sol_in.assignments.size() < 2)
            return make_tuple(false, move);
        
        uniform_int_distribution<int> d(0, (int)sol_in.assignments.size() - 1);
        int i = d(rng), j = d(rng);
        if (i == j)
            return make_tuple(false, move);
        
        const auto &a = sol_in.assignments[i];
        const auto &b = sol_in.assignments[j];

        OptimalSolution cand = sol_in;

        cand.assignments[i].teacher_id = b.teacher_id;
        cand.assignments[i].day = b.day;
        cand.assignments[i].period = b.period;
        cand.assignments[i].classroom_id = b.classroom_id;

        cand.assignments[j].teacher_id = a.teacher_id;
        cand.assignments[j].day = a.day;
        cand.assignments[j].period = a.period;
        cand.assignments[j].classroom_id = a.classroom_id;

        SolIndex idx = build_index(cand, data);
        if (!check_course_teacher_bounds(idx, data))
            return make_tuple(false, move);

        if (IsFeasible(cand.assignments[i].teacher_id, a.course_id, a.section_id, cand.assignments[i].day, cand.assignments[i].period, cand.assignments[i].classroom_id, cand, idx, data) &&
            IsFeasible(cand.assignments[j].teacher_id, b.course_id, b.section_id, cand.assignments[j].day, cand.assignments[j].period, cand.assignments[j].classroom_id, cand, idx, data))
        {
            sol_out = cand;
            move.indices = {i, j};
            move.old_values = {a.teacher_id, a.day, a.period, a.classroom_id, b.teacher_id, b.day, b.period, b.classroom_id};
            move.new_values = {b.teacher_id, b.day, b.period, b.classroom_id, a.teacher_id, a.day, a.period, a.classroom_id};
            return make_tuple(true, move);
        }
        return make_tuple(false, move);
    }

    static tuple<bool, Move> move_block_relocate(
        OptimalSolution &sol_out,
        const OptimalSolution &sol_in,
        const ProblemData &data)
    {
        Move move;
        move.type = Move::BLOCK_RELOCATE;
        
        if (sol_in.assignments.empty())
            return make_tuple(false, move);
        
        uniform_int_distribution<int> d(0, (int)sol_in.assignments.size() - 1);
        int idx_assign = d(rng);
        const auto &a = sol_in.assignments[idx_assign];
        move.indices.push_back(idx_assign);
        
        int attempts = 6;
        uniform_int_distribution<int> ldist(0, (int)data.classrooms.days.size() - 1);
        uniform_int_distribution<int> pdist(0, (int)data.classrooms.periods.size() - 1);
        for (int t = 0; t < attempts; ++t)
        {
            string nd = data.classrooms.days[ldist(rng)];
            string np = data.classrooms.periods[pdist(rng)];
            if (nd == a.day && np == a.period)
                continue;
            OptimalSolution cand = sol_in;
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
                sol_out = cand;
                move.old_values = {a.teacher_id, a.day, a.period, a.classroom_id};
                move.new_values = {a.teacher_id, nd, np, new_classroom_id};
                return make_tuple(true, move);
            }
        }
        return make_tuple(false, move);
    }

    static tuple<bool, Move> move_block_swap(
        OptimalSolution &sol_out,
        const OptimalSolution &sol_in,
        const ProblemData &data)
    {
        Move move;
        move.type = Move::BLOCK_SWAP;
        
        if (sol_in.assignments.size() < 2)
            return make_tuple(false, move);
        
        uniform_int_distribution<int> d(0, (int)sol_in.assignments.size() - 1);
        int i = d(rng), j = d(rng);
        if (i == j)
            return make_tuple(false, move);
        
        const auto &A = sol_in.assignments[i];
        const auto &B = sol_in.assignments[j];

        OptimalSolution cand = sol_in;
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
            return make_tuple(false, move);

        if (IsFeasibleBlock(cand.assignments[i].teacher_id, A.course_id, A.section_id, cand.assignments[i].day, cand.assignments[i].period, cand.assignments[i].classroom_id, cand, idx, data) &&
            IsFeasibleBlock(cand.assignments[j].teacher_id, B.course_id, B.section_id, cand.assignments[j].day, cand.assignments[j].period, cand.assignments[j].classroom_id, cand, idx, data))
        {
            sol_out = cand;
            move.indices = {i, j};
            move.old_values = {A.teacher_id, A.day, A.period, A.classroom_id, B.teacher_id, B.day, B.period, B.classroom_id};
            move.new_values = {B.teacher_id, B.day, B.period, B.classroom_id, A.teacher_id, A.day, A.period, A.classroom_id};
            return make_tuple(true, move);
        }
        return make_tuple(false, move);
    }

}

// ---------- Main Phase3 ----------
OptimalSolution find_optimal_solution(const ProblemData &data, const InitialSolution &initial)
{
    unordered_map<string, const Teacher*> teacher_map = build_teacher_map(data);
    unordered_map<string, unordered_map<string, int>> time_pref_map = build_time_pref_map(data);

    OptimalSolution initial_sol = initial;
    OptimalSolution current = initial;
    OptimalSolution best = initial;

    current.objective_value = Evaluate(current, data, teacher_map, time_pref_map, initial_sol);
    best.objective_value = current.objective_value;

    SignatureBasedTabu sig_tabu(50, 100);
    
    using MoveFn = tuple<bool, Move> (*)(OptimalSolution &, const OptimalSolution &, const ProblemData &);
    vector<MoveFn> neighborhoods = {
        &move_single_change,
        &move_teacher_swap,
        &move_pair_swap,
        &move_block_relocate,
        &move_block_swap};

    double T = 1.0;
    double alpha = 0.98;
    int max_iterations = 1200;
    int moves_per_nb = 40;
    int limit_no_improv = 300;

    deque<double> recent_objs;
    int numb_iter_no_improv = 0;
    
    // Debug: track feasible moves
    int total_feasible_moves = 0;
    int accepted_moves = 0;
    int tabu_rejected = 0;
    int sa_rejected = 0;
    int infeasible_moves = 0;
    const char* move_names[] = {"SINGLE_CHANGE", "TEACHER_SWAP", "PAIR_SWAP", "BLOCK_RELOCATE", "BLOCK_SWAP"};

    for (int iter = 0; iter < max_iterations; ++iter)
    {
        // Stop if temperature is too low
        if (T < 1e-4)
        {
            cout << "[Phase3] Stopping: Temperature " << T << " < 1e-4\n";
            break;
        }
        
        if (iter == 0 || iter % 10 == 0)
        {
            cout << "[Phase3] ===== Iteration " << iter 
                 << " | Current=" << current.objective_value 
                 << " | Best=" << best.objective_value 
                 << " | Temp=" << T 
                 << " | Alpha=" << alpha << " =====\n";
        }
        
        bool any_improved = false;

        for (size_t nb = 0; nb < neighborhoods.size(); ++nb)
        {
            bool improved_in_nb = false;
            int nb_feasible = 0;
            int nb_tabu = 0;
            int nb_sa_reject = 0;

            if (iter == 0 || iter % 10 == 0)
            {
                cout << "[Phase3]   Trying neighborhood " << nb << " (" << move_names[nb] << ")\n";
            }

            for (int mv = 0; mv < moves_per_nb; ++mv)
            {
                OptimalSolution cand;
                auto move_result = neighborhoods[nb](cand, current, data);
                bool move_success = get<0>(move_result);
                if (!move_success)
                {
                    infeasible_moves++;
                    if (iter == 0 && mv < 5)
                    {
                        cout << "[Phase3]     Move " << mv << " failed: not feasible\n";
                    }
                    continue;
                }

                Move move = get<1>(move_result);
                total_feasible_moves++;
                nb_feasible++;

                double cand_score = Evaluate(cand, data, teacher_map, time_pref_map, initial_sol);
                
                // Extract signatures from solution changes
                auto sigs = extract_signatures_from_changes(current, cand);
                string sig_tc = get<0>(sigs);
                string sig_ct = get<1>(sigs);
                
                if (sig_tabu.is_tabu(sig_tc, sig_ct, best.objective_value, cand_score))
                {
                    tabu_rejected++;
                    nb_tabu++;
                    if (iter == 0 && nb_feasible <= 3)
                    {
                        cout << "[Phase3]     Move " << mv << " (" << move_names[move.type] 
                             << ") rejected: TABU | Score=" << cand_score 
                             << " | SigTC=" << (sig_tc.empty() ? "none" : sig_tc.substr(0, 30))
                             << " | SigCT=" << (sig_ct.empty() ? "none" : sig_ct.substr(0, 30)) << "\n";
                    }
                    continue;
                }
                double delta = cand_score - current.objective_value;

                bool accept = false;
                double prob = 1.0;
                if (delta >= 0)
                {
                    accept = true;
                    prob = 1.0;
                }
                else
                {
                    recent_objs.push_back(current.objective_value);
                    if (recent_objs.size() > 100)
                        recent_objs.pop_front();
                    
                    prob = compute_acceptance_probability(
                        delta,
                        current.objective_value,
                        recent_objs,
                        T
                    );
                    
                    uniform_real_distribution<double> u(0.0, 1.0);
                    double rand_val = u(rng);
                    if (rand_val < prob)
                    {
                        accept = true;
                }
                    else
                    {
                        sa_rejected++;
                        nb_sa_reject++;
                        if (iter == 0 && nb_feasible <= 3)
                        {
                            cout << "[Phase3]     Move " << mv << " (" << move_names[move.type] 
                                 << ") rejected: SA | Delta=" << delta 
                                 << " | Score=" << cand_score 
                                 << " | Prob=" << prob 
                                 << " | Rand=" << rand_val << "\n";
                        }
                    }
                }

                if (accept)
                {
                    accepted_moves++;
                    
                    cout << "[Phase3]     ✓ Move ACCEPTED: " << move_names[move.type] 
                         << " | Delta=" << delta 
                         << " | Score=" << cand_score 
                         << " | Current=" << current.objective_value
                         << " | Temp=" << T
                         << " | Prob=" << prob << "\n";
                    
                    sig_tabu.add_tabu(sig_tc, sig_ct);

                    current = cand;
                    current.objective_value = cand_score;

                    if (cand_score > best.objective_value)
                    {
                        cout << "[Phase3]     ★ NEW BEST! " << cand_score << " (was " << best.objective_value << ")\n";
                        best = cand;
                        best.objective_value = cand_score;
                        numb_iter_no_improv = 0;
                    }
                    else
                    {
                        ++numb_iter_no_improv;
                    }

                    improved_in_nb = true;
                    any_improved = true;
                    nb = 0;
                    break;
                }
            } 

            if (!improved_in_nb)
            {
                if (iter == 0 || iter % 10 == 0)
                {
                    cout << "[Phase3]   Neighborhood " << nb << " done: Feasible=" << nb_feasible 
                         << " | Tabu=" << nb_tabu 
                         << " | SA_Reject=" << nb_sa_reject 
                         << " | Accepted=0\n";
                }
                continue;
            }
        } // neighborhoods

        if (!any_improved)
        {
            if (iter == 0 || iter % 10 == 0)
            {
                cout << "[Phase3]   No improvement in iteration, trying diversification shakes...\n";
            }
            
            // diversification: random shakes
            int shakes = 4;
            for (int s = 0; s < shakes; ++s)
            {
                OptimalSolution cand;
                auto move_result = move_block_relocate(cand, current, data);
                bool move_success = get<0>(move_result);
                if (!move_success)
                {
                    move_result = move_teacher_swap(cand, current, data);
                    move_success = get<0>(move_result);
                }
                if (!move_success)
                {
                    move_result = move_pair_swap(cand, current, data);
                    move_success = get<0>(move_result);
                }
                if (!move_success)
                {
                    if (iter == 0 && s < 2)
                    {
                        cout << "[Phase3]     Shake " << s << " failed: no feasible move\n";
                    }
                    continue;
                }

                Move move = get<1>(move_result);

                double cand_score = Evaluate(cand, data, teacher_map, time_pref_map, initial_sol);

                // Extract signatures from solution changes
                auto sigs = extract_signatures_from_changes(current, cand);
                string sig_tc = get<0>(sigs);
                string sig_ct = get<1>(sigs);

                Move shake_move = get<1>(move_result);
                
                if (sig_tabu.is_tabu(sig_tc, sig_ct, best.objective_value, cand_score))
                {
                    if (iter == 0 && s < 2)
                    {
                        cout << "[Phase3]     Shake " << s << " rejected: TABU\n";
                    }
                    continue;
                }

                double delta = cand_score - current.objective_value;
                
                recent_objs.push_back(current.objective_value);
                if (recent_objs.size() > 100)
                    recent_objs.pop_front();
                
                double prob = compute_acceptance_probability(
                    delta,
                    current.objective_value,
                    recent_objs,
                    T
                );
                
                uniform_real_distribution<double> u(0.0, 1.0);
                double rand_val = u(rng);
                if (rand_val < prob)
                {
                    cout << "[Phase3]     ✓ Shake " << s << " ACCEPTED: " << move_names[shake_move.type]
                         << " | Delta=" << delta 
                         << " | Score=" << cand_score << "\n";
                    
                    sig_tabu.add_tabu(sig_tc, sig_ct);

                    current = cand;
                    current.objective_value = cand_score;
                    
                    if (cand_score > best.objective_value)
                    {
                        cout << "[Phase3]     ★ NEW BEST from shake! " << cand_score << "\n";
                        best = cand;
                        best.objective_value = cand_score;
                        numb_iter_no_improv = 0;
                    }
                    else
                        ++numb_iter_no_improv;
                    break;
                }
                else
                {
                    if (iter == 0 && s < 2)
                    {
                        cout << "[Phase3]     Shake " << s << " rejected: SA | Delta=" << delta 
                             << " | Prob=" << prob << " | Rand=" << rand_val << "\n";
                    }
                }
            }
        }
        
        if (!any_improved && (iter == 0 || iter % 10 == 0))
        {
            cout << "[Phase3]   No improvement in this iteration\n";
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
            numb_iter_no_improv = 0;
            shuffle(current.assignments.begin(), current.assignments.end(), rng);
            for (int k = 0; k < (int)current.assignments.size() / 12; ++k)
            {
                OptimalSolution cand;
                auto move_result = move_single_change(cand, current, data);
                if (get<0>(move_result))
                    current = cand;
            }
        }

        if (iter % 50 == 0)
        {
            cout << "[Phase3] ===== Summary Iter " << iter << " =====\n";
            cout << "[Phase3]   Best=" << best.objective_value
                 << " | Current=" << current.objective_value
                 << " | Temp=" << T
                 << " | Alpha=" << alpha << "\n";
            cout << "[Phase3]   TabuSize(TC/CT)=" << sig_tabu.tabu_set_tc.size() << "/" << sig_tabu.tabu_set_ct.size()
                 << " | NoImprove=" << numb_iter_no_improv << "\n";
            cout << "[Phase3]   Moves: Feasible=" << total_feasible_moves
                 << " | Accepted=" << accepted_moves
                 << " | TabuRejected=" << tabu_rejected
                 << " | SA_Rejected=" << sa_rejected
                 << " | Infeasible=" << infeasible_moves << "\n";
            cout << "[Phase3]   Acceptance rate: " 
                 << (total_feasible_moves > 0 ? (100.0 * accepted_moves / total_feasible_moves) : 0.0) << "%\n";
        }
    } // main loop

    cout << "[Phase3] ===== FINAL SUMMARY =====\n";
    cout << "[Phase3]   Best objective: " << best.objective_value << "\n";
    cout << "[Phase3]   Total assignments: " << best.assignments.size() << "\n";
    cout << "[Phase3]   Moves statistics:\n";
    cout << "[Phase3]     - Total feasible moves: " << total_feasible_moves << "\n";
    cout << "[Phase3]     - Accepted moves: " << accepted_moves << "\n";
    cout << "[Phase3]     - Tabu rejected: " << tabu_rejected << "\n";
    cout << "[Phase3]     - SA rejected: " << sa_rejected << "\n";
    cout << "[Phase3]     - Infeasible moves: " << infeasible_moves << "\n";
    if (total_feasible_moves > 0)
    {
        cout << "[Phase3]   Acceptance rate: " << (100.0 * accepted_moves / total_feasible_moves) << "%\n";
        cout << "[Phase3]   Tabu rejection rate: " << (100.0 * tabu_rejected / total_feasible_moves) << "%\n";
        cout << "[Phase3]   SA rejection rate: " << (100.0 * sa_rejected / total_feasible_moves) << "%\n";
    }
    cout << "[Phase3] =========================\n";
    return best;
}

// ---------- Evaluate Initial Solution ----------
double evaluate_initial_solution(const ProblemData& data, const InitialSolution& init_sol)
{
    unordered_map<string, const Teacher*> teacher_map;
    for (const auto &t : data.teachers)
        teacher_map[t.id] = &t;
    
    unordered_map<string, unordered_map<string, int>> time_pref_map;
    for (const auto &t : data.teachers)
    {
        for (const auto &tp : t.time_pref)
        {
            string key = tp.day + "|" + tp.period;
            time_pref_map[t.id][key] = tp.score;
        }
    }
    
    OptimalSolution sol(init_sol);
    OptimalSolution initial_sol(init_sol);
    
    return Evaluate(sol, data, teacher_map, time_pref_map, initial_sol);
}
