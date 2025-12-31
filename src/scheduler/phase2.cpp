#include "phase2.h"
#include "phase_common.h"
#include "ortools/sat/cp_model.h"
#include "ortools/sat/cp_model_solver.h"
#include <iostream>
#include <vector>
#include <map>
#include <tuple>
#include <functional>
#include <algorithm>
#include <random>
#include <chrono>
#include <unordered_set>
#include <unordered_map>
#include <set>
#include <cmath>
#include <sstream>
#include <numeric>
#include <cassert>

using namespace operations_research;
using namespace operations_research::sat;
using namespace std;

using DecisionVariableKey = tuple<int, int, int, int, int>;

struct KeyHash {
    size_t operator()(const DecisionVariableKey& key) const {
        size_t h1 = hash<int>{}(get<0>(key));
        size_t h2 = hash<int>{}(get<1>(key));
        size_t h3 = hash<int>{}(get<2>(key));
        size_t h4 = hash<int>{}(get<3>(key));
        size_t h5 = hash<int>{}(get<4>(key));
        return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3) ^ (h5 << 4);
    }
};

using DecisionVariableMap = unordered_map<DecisionVariableKey, BoolVar, KeyHash>;
using EligibilityMatrix = vector<vector<bool>>;
using TeacherAvailabilityMatrix = vector<vector<vector<bool>>>; // [teacher][day][period] = available
using TeacherWorkloadMap = unordered_map<string, int>;
using TimeslotSet = unordered_set<string>;
using AssignmentMapping = map<pair<string, string>, pair<string, pair<string, string>>>;
using SolutionPool = vector<InitialSolution>;

struct PrecomputedData {
    map<pair<string, string>, int> section_seats;
    map<pair<string, string>, int> section_periods;
    std::unordered_map<std::string, std::vector<int>> teacher_eligible_courses;
    std::unordered_map<std::string, int> classroom_capacity;
    std::unordered_map<std::string, int> course_eligible_teacher_count;
    std::unordered_map<std::string, int> course_id_to_idx;
};

struct ShuffledIndices {
    vector<int> teacher_order;
    vector<int> course_order;
    vector<int> day_order;
    vector<int> period_order;
    vector<vector<int>> section_orders;
};

static mt19937 create_random_generator(int seed = -1) {
    if (seed < 0) {
        return mt19937(chrono::steady_clock::now().time_since_epoch().count());
    }
    return mt19937(seed);
}

static int random_int_in_range(mt19937 &rng, int min, int max) {
    if (min > max) return min;
    uniform_int_distribution<int> dist(min, max);
    return dist(rng);
}

// Forward declarations
static void extract_solution(const CpSolverResponse &response, const DecisionVariableMap &Y, const ProblemData &data,
                             const ShuffledIndices &shuffled, InitialSolution &sol);
static bool validate_solution_completeness(const InitialSolution &sol, const ProblemData &data);

static PrecomputedData precompute_data(const ProblemData &data) {
    PrecomputedData precomp;
    
    // Precompute section data
    for (const auto &course : data.courses) {
        for (const auto &section : course.sections) {
            precomp.section_seats[{course.id, section.id}] = section.required_seats;
            precomp.section_periods[{course.id, section.id}] = section.required_periods;
        }
    }
    
    // Build course ID to index map
    for (int j = 0; j < (int)data.courses.size(); ++j) {
        precomp.course_id_to_idx[data.courses[j].id] = j;
    }

    // Precompute teacher eligibility efficiently
    for (size_t i = 0; i < data.teachers.size(); ++i) {
        const auto &teacher = data.teachers[i];
        for (const auto &course_id : teacher.eligible_courses) {
            auto it = precomp.course_id_to_idx.find(course_id);
            if (it != precomp.course_id_to_idx.end()) {
                int j = it->second;
                precomp.teacher_eligible_courses[teacher.id].push_back(j);
                precomp.course_eligible_teacher_count[course_id]++;
            }
        }
    }
    
    // Precompute classroom capacity
    for (const auto &room : data.classrooms.classrooms) {
        precomp.classroom_capacity[room.id] = room.capacity;
    }
    
    return precomp;
}

static int get_required_seats_fast(const PrecomputedData &precomp, 
                                    const string &course_id, 
                                    const string &section_id) {
    auto it = precomp.section_seats.find({course_id, section_id});
    return (it != precomp.section_seats.end()) ? it->second : 0;
}

static int get_required_periods_fast(const PrecomputedData &precomp,
                                      const string &course_id,
                                      const string &section_id) {
    auto it = precomp.section_periods.find({course_id, section_id});
    return (it != precomp.section_periods.end()) ? it->second : 1;
}

static int count_total_sections(const ProblemData &data) {
    int total = 0;
    for (const auto &c : data.courses)
        total += (int)c.sections.size();
    return total;
}

static double compute_stddev(const vector<int> &vals) {
    if (vals.empty()) return 0.0;
    
    double mean = accumulate(vals.begin(), vals.end(), 0.0) / vals.size();
    double variance = 0.0;
    for (int v : vals) {
        variance += (v - mean) * (v - mean);
    }
    variance /= vals.size();
    
    return sqrt(variance);
}

static double clamp_value(double value, double min_val, double max_val) {
    return max(min_val, min(max_val, value));
}

template<typename T>
static void shuffle_indices(vector<T> &indices, mt19937 &rng) {
    iota(indices.begin(), indices.end(), 0);
    shuffle(indices.begin(), indices.end(), rng);
}

// Helper: Check if teacher is eligible for course (using common utility)
// Note: is_teacher_eligible_for_course is now in phase_common.h


static ShuffledIndices create_shuffled_indices(const ProblemData &data, const PrecomputedData &precomp, mt19937 &rng) {
    ShuffledIndices shuffled;
    shuffle_indices(shuffled.teacher_order = vector<int>(data.teachers.size()), rng);
    shuffle_indices(shuffled.course_order = vector<int>(data.courses.size()), rng);
    shuffle_indices(shuffled.day_order = vector<int>(data.classrooms.days.size()), rng);
    shuffle_indices(shuffled.period_order = vector<int>(data.classrooms.periods.size()), rng);
    
    shuffled.section_orders.resize(data.courses.size());
    for (int j = 0; j < (int)data.courses.size(); ++j) {
        int orig_j = shuffled.course_order[j];
        const auto &course = data.courses[orig_j];
        vector<pair<int, pair<int, int>>> section_scores;
        
        // Optimize: use precomputed teacher count
        int eligible_count = 0;
        auto it = precomp.course_eligible_teacher_count.find(course.id);
        if (it != precomp.course_eligible_teacher_count.end()) {
            eligible_count = it->second;
        }
        
        for (int k = 0; k < (int)course.sections.size(); ++k) {
            section_scores.push_back({k, {course.sections[k].required_periods, eligible_count}});
        }
        sort(section_scores.begin(), section_scores.end(),
             [](const auto &a, const auto &b) {
                 return a.second.first != b.second.first ? a.second.first > b.second.first : a.second.second < b.second.second;
             });
        shuffled.section_orders[j].resize(section_scores.size());
        for (int k = 0; k < (int)section_scores.size(); ++k) shuffled.section_orders[j][k] = section_scores[k].first;
    }
    return shuffled;
}

static void configure_solver(Model &sat_model, int time_limit_seconds, int random_seed, bool stop_after_first_solution = false) {
    ostringstream param_stream;
    param_stream << "max_time_in_seconds:" << time_limit_seconds 
                 << " num_search_workers:4 "  // Reduced from 8 to 4 for better performance on smaller problems
                 << "cp_model_presolve:true "
                 << "linearization_level:1 "
                 << "log_search_progress:false "  // Disable logging for better performance
                 << "random_seed:" << random_seed;
    if (stop_after_first_solution) param_stream << " stop_after_first_solution:true";
    sat_model.Add(NewSatParameters(param_stream.str()));
}

static bool is_solution_feasible(const CpSolverStatus &status) {
    return status == CpSolverStatus::FEASIBLE || status == CpSolverStatus::OPTIMAL;
}

static int compute_total_periods(const ProblemData &data, const ShuffledIndices &shuffled) {
    int total = 0;
    for (int j = 0; j < (int)data.courses.size(); ++j) {
        for (const auto &section : data.courses[shuffled.course_order[j]].sections) total += section.required_periods;
    }
    return total;
}

static int compute_max_possible_workload(const ProblemData &data, const ShuffledIndices &shuffled,
                                         const EligibilityMatrix &eligible, int teacher_idx) {
    int max_workload = 0;
    for (int j = 0; j < (int)data.courses.size(); ++j) {
        if (!eligible[teacher_idx][j]) continue;
        for (const auto &section : data.courses[shuffled.course_order[j]].sections) max_workload += section.required_periods;
    }
    return max_workload;
}

static InitialSolution solve_and_extract_solution(CpModelBuilder &model, const ProblemData &data, const ShuffledIndices &shuffled,
                                                  const DecisionVariableMap &Y, int time_limit_seconds, int random_seed,
                                                  bool stop_after_first_solution = true) {
    Model sat_model;
    configure_solver(sat_model, time_limit_seconds, random_seed, stop_after_first_solution);
    auto response = SolveCpModel(model.Build(), &sat_model);
    if (!is_solution_feasible(response.status())) return InitialSolution();
    InitialSolution sol;
    extract_solution(response, Y, data, shuffled, sol);
    return (sol.assignments.empty() || !validate_solution_completeness(sol, data)) ? InitialSolution() : sol;
}

static map<pair<int, int>, BoolVar> collect_section_assignments(const CpSolverResponse &response, const DecisionVariableMap &Y) {
    map<pair<int, int>, BoolVar> section_assignments;
    for (const auto &kv : Y) {
        if (SolutionBooleanValue(response, kv.second)) {
            int i, j, k, l, m0;
            tie(i, j, k, l, m0) = kv.first;
            assert(section_assignments.find({j, k}) == section_assignments.end() && "Model error: duplicate section assignment");
            section_assignments[{j, k}] = kv.second;
        }
    }
    return section_assignments;
}

static void add_no_good_cut(CpModelBuilder &model, const map<pair<int, int>, BoolVar> &section_assignments, mt19937 &rng) {
    if (section_assignments.empty()) return;
    int num_sections = (int)section_assignments.size();
    LinearExpr diff = 0;
    
    if (num_sections > 100) {
        vector<pair<pair<int, int>, BoolVar>> vec(section_assignments.begin(), section_assignments.end());
        shuffle(vec.begin(), vec.end(), rng);
        int sample_size = min(50, num_sections);
        for (int i = 0; i < sample_size; ++i) diff += (1 - vec[i].second);
        model.AddGreaterOrEqual(diff, 1);
    } else {
        for (const auto &[section_key, var] : section_assignments) diff += (1 - var);
        int min_diff = (num_sections < 15) ? 1 : (num_sections < 40) ? 2 : 3;
        model.AddGreaterOrEqual(diff, min_diff);
    }
}

static void extract_solution(const CpSolverResponse &response, const DecisionVariableMap &Y, const ProblemData &data,
                             const ShuffledIndices &shuffled, InitialSolution &sol) {
    for (const auto &kv : Y) {
        if (SolutionBooleanValue(response, kv.second)) {
            int i, j, k, l, m0;
            tie(i, j, k, l, m0) = kv.first;
            int orig_i = shuffled.teacher_order[i], orig_j = shuffled.course_order[j], orig_k = shuffled.section_orders[j][k];
            int orig_l = shuffled.day_order[l];
            // m0 is now the original period index (not shuffled), so use it directly
            int orig_m0 = m0;
            InitialSolution::Assignment a;
            a.teacher_id = data.teachers[orig_i].id;
            a.course_id = data.courses[orig_j].id;
            a.section_id = data.courses[orig_j].sections[orig_k].id;
            a.day = data.classrooms.days[orig_l];
            a.period = data.classrooms.periods[orig_m0];
            a.classroom_id = "";
            a.initial_teacher = a.teacher_id;
            a.initial_timeslot = ::format_timeslot_key(a.day, a.period);
            sol.assignments.push_back(a);
        }
    }
}

static bool validate_solution_completeness(const InitialSolution &sol, const ProblemData &data) {
    unordered_map<string, unordered_set<string>> assigned;
    for (const auto &a : sol.assignments) assigned[a.course_id].insert(a.section_id);
    for (const auto &course : data.courses) {
        auto it = assigned.find(course.id);
        if (it == assigned.end()) return false;
        for (const auto &section : course.sections) {
            if (it->second.count(section.id) == 0) return false;
        }
    }
    return true;
}

static size_t compute_solution_hash(const InitialSolution &sol) {
    size_t hash_val = 0;
    for (const auto &a : sol.assignments) {
        hash_val ^= hash<string>{}(a.teacher_id + "|" + a.course_id + "|" + a.section_id + "|" + a.day + "|" + a.period) + 0x9e3779b9 + (hash_val << 6) + (hash_val >> 2);
    }
    return hash_val;
}

static double compute_diversity(const InitialSolution &sol1, const InitialSolution &sol2, bool approximate = false, int sample_size = 10) {
    if (sol1.assignments.size() != sol2.assignments.size() || sol1.assignments.empty())
        return sol1.assignments.empty() ? 0.0 : 1.0;
    
    if (approximate && sol1.assignments.size() > sample_size) {
        size_t hash1 = compute_solution_hash(sol1), hash2 = compute_solution_hash(sol2);
        if (hash1 == hash2) return 0.0;
        int hamming = 0;
        for (size_t i = 0; i < 64 && i < sol1.assignments.size(); ++i) {
            hamming += ((hash1 >> i) & 1) != ((hash2 >> i) & 1);
        }
        return (double)hamming / 64.0;
    }
    
    int teacher_diff = 0, timeslot_diff = 0, total = 0;
    AssignmentMapping map1, map2;
    for (const auto &a : sol1.assignments) map1[{a.course_id, a.section_id}] = {a.teacher_id, {a.day, a.period}};
    for (const auto &a : sol2.assignments) map2[{a.course_id, a.section_id}] = {a.teacher_id, {a.day, a.period}};
    for (const auto &kv : map1) {
        auto it = map2.find(kv.first);
        if (it == map2.end()) { teacher_diff++; timeslot_diff++; }
        else {
            if (kv.second.first != it->second.first) teacher_diff++;
            if (kv.second.second != it->second.second) timeslot_diff++;
        }
        total++;
    }
    
    if (total == 0) return 0.0;
    return 0.5 * ((double)teacher_diff / total) + 0.5 * ((double)timeslot_diff / total);
}

static bool is_solution_diverse_enough(const InitialSolution &sol, const SolutionPool &pool, double min_diversity = 0.1) {
    // Use approximate mode early for better performance: when pool has >3 solutions or solution has >15 assignments
    bool approximate = pool.size() > 3 || sol.assignments.size() > 15;
    int sample_size = approximate ? 20 : 10; // Smaller sample for faster computation
    
    for (const auto &prev : pool) {
        if (compute_diversity(sol, prev, approximate, sample_size) < min_diversity) return false;
    }
    return true;
}

static double compute_adaptive_diversity_threshold(int num_sections) {
    if (num_sections <= 10)
        return 0.15;
    else if (num_sections <= 30)
        return 0.1;
    else
        return 0.05;
}

struct ViableSlot {
    int i, j, k, l, m0;
    int r; // required_periods
};

static vector<ViableSlot> precompute_viable_slots(const ProblemData &data, const PrecomputedData &precomp,
                                                  const ShuffledIndices &shuffled, const EligibilityMatrix &eligible,
                                                  const TeacherAvailabilityMatrix &teacher_available) {
    vector<ViableSlot> viable;
    viable.reserve(10000); // Pre-allocate to reduce reallocations
    int I = (int)data.teachers.size(), J = (int)data.courses.size(), L = (int)data.classrooms.days.size(), M = (int)data.classrooms.periods.size();
    unordered_set<string> seen_sections;
    
    // Pre-compute max classroom capacity for early filtering
    int max_classroom_capacity = 0;
    for (const auto &room : data.classrooms.classrooms) {
        max_classroom_capacity = max(max_classroom_capacity, room.capacity);
    }
    
    for (int i = 0; i < I; ++i) {
        for (int j = 0; j < J; ++j) {
            if (!eligible[i][j]) continue;
            int orig_j = shuffled.course_order[j], S_j = (int)data.courses[orig_j].sections.size();
            for (int k = 0; k < S_j; ++k) {
                int orig_k = shuffled.section_orders[j][k];
                int r = data.courses[orig_j].sections[orig_k].required_periods;
                int seats = data.courses[orig_j].sections[orig_k].required_seats;
                
                // Early filter: check if any room has sufficient capacity
                if (seats > max_classroom_capacity) continue;
                
                string section_key = to_string(i) + "|" + to_string(j) + "|" + to_string(k);
                if (seen_sections.count(section_key)) continue;
                seen_sections.insert(section_key);
                
                for (int l = 0; l < L; ++l) {
                    for (int orig_m0 = 0; orig_m0 + r <= M; ++orig_m0) {
                        bool available = true;
                        for (int p = 0; p < r && available; ++p) {
                            int period_idx = orig_m0 + p;
                            if (period_idx >= M || !teacher_available[i][l][period_idx])
                                available = false;
                        }
                        if (available) {
                            viable.push_back({i, j, k, l, orig_m0, r});
                        }
                    }
                }
            }
        }
    }
    return viable;
}

static void add_section_assignment_constraint(CpModelBuilder &model, const ProblemData &data, const ShuffledIndices &shuffled,
                                               const EligibilityMatrix &eligible, const DecisionVariableMap &Y, const vector<ViableSlot> &viable) {
    for (int j = 0; j < (int)data.courses.size(); ++j) {
        int orig_j = shuffled.course_order[j], S_j = (int)data.courses[orig_j].sections.size();
        for (int k = 0; k < S_j; ++k) {
            LinearExpr sum = 0;
            for (const auto &slot : viable) {
                if (slot.j == j && slot.k == k) {
                    auto it = Y.find({slot.i, slot.j, slot.k, slot.l, slot.m0});
                    if (it != Y.end()) sum += it->second;
                }
            }
            model.AddEquality(sum, 1);
        }
    }
}

static void add_teacher_time_conflict_constraint(CpModelBuilder &model, const ProblemData &data, const ShuffledIndices &shuffled,
                                                 const EligibilityMatrix &eligible, const DecisionVariableMap &Y, const vector<ViableSlot> &viable) {
    int I = (int)data.teachers.size(), L = (int)data.classrooms.days.size(), M = (int)data.classrooms.periods.size();
    
    // Build constraints efficiently using map: (teacher_i, day_l, period_m) -> LinearExpr
    map<tuple<int, int, int>, LinearExpr> teacher_slot_exprs;
    
    for (const auto &slot : viable) {
        auto it = Y.find({slot.i, slot.j, slot.k, slot.l, slot.m0});
        if (it == Y.end()) continue;
        
        // For each period this slot covers, add to the constraint
        for (int p = 0; p < slot.r; ++p) {
            int period_m = slot.m0 + p;
            if (period_m >= M) continue;
            
            auto key = make_tuple(slot.i, slot.l, period_m);
            teacher_slot_exprs[key] += it->second;
        }
    }
    
    // Add constraints: at most 1 assignment per teacher per time slot
    for (const auto &[key, expr] : teacher_slot_exprs) {
        model.AddLessOrEqual(expr, 1);
    }
}

static void add_course_section_overlap_constraint(CpModelBuilder &model, const ProblemData &data, const ShuffledIndices &shuffled,
                                                  const EligibilityMatrix &eligible, const DecisionVariableMap &Y, const vector<ViableSlot> &viable) {
    int J = (int)data.courses.size(), L = (int)data.classrooms.days.size(), M = (int)data.classrooms.periods.size();
    
    // Hard constraint: Sections of same course cannot overlap
    // Use map to build constraints efficiently: (course_j, day_l, period_m) -> LinearExpr
    map<tuple<int, int, int>, LinearExpr> course_slot_exprs;
    
    for (const auto &slot : viable) {
        int orig_j = shuffled.course_order[slot.j];
        int S_j = (int)data.courses[orig_j].sections.size();
        if (S_j <= 1) continue; // No overlap possible with single section
        
        // For each period this slot covers, add to the constraint
        for (int p = 0; p < slot.r; ++p) {
            int period_m = slot.m0 + p;
            if (period_m >= M) continue;
            
            auto key = make_tuple(slot.j, slot.l, period_m);
            auto it = Y.find({slot.i, slot.j, slot.k, slot.l, slot.m0});
            if (it != Y.end()) {
                course_slot_exprs[key] += it->second;
            }
        }
    }
    
    // Add constraints: at most 1 section per course per time slot
    for (const auto &[key, expr] : course_slot_exprs) {
        model.AddLessOrEqual(expr, 1);
    }
}

static void add_workload_constraints(CpModelBuilder &model, const ProblemData &data, const ShuffledIndices &shuffled,
                                     const EligibilityMatrix &eligible, const DecisionVariableMap &Y, const vector<ViableSlot> &viable,
                                     vector<IntVar> &teacher_workload_vars, double soft_weight) {
    int I = (int)data.teachers.size();
    teacher_workload_vars.clear();
    teacher_workload_vars.resize(I);
    bool need_intvar = soft_weight > 0.0;
    
    for (int i = 0; i < I; ++i) {
        LinearExpr workload = 0;
        for (const auto &slot : viable) {
            if (slot.i == i) {
                auto it = Y.find({slot.i, slot.j, slot.k, slot.l, slot.m0});
                if (it != Y.end()) workload += slot.r * it->second;
            }
        }
        int orig_i = shuffled.teacher_order[i];
        if (data.teachers[orig_i].max_periods > 0) {
            int max_workload = compute_max_possible_workload(data, shuffled, eligible, i);
            teacher_workload_vars[i] = model.NewIntVar({0, max_workload});
            model.AddEquality(teacher_workload_vars[i], workload);
            model.AddLessOrEqual(teacher_workload_vars[i], data.teachers[orig_i].max_periods);
        } else if (need_intvar) {
            int max_workload = compute_max_possible_workload(data, shuffled, eligible, i);
            teacher_workload_vars[i] = model.NewIntVar({0, max_workload});
            model.AddEquality(teacher_workload_vars[i], workload);
        }
    }
}

static EligibilityMatrix build_eligibility_matrix(const ProblemData &data, const PrecomputedData &precomp, const ShuffledIndices &shuffled) {
    int I = (int)data.teachers.size(), J = (int)data.courses.size();
    EligibilityMatrix eligible(I, vector<bool>(J, false));
    
    // Create inverse mapping for course indices: original_idx -> shuffled_idx
    vector<int> orig_to_shuffled_course(J);
    for (int j = 0; j < J; ++j) {
        orig_to_shuffled_course[shuffled.course_order[j]] = j;
    }
    
    for (int i = 0; i < I; ++i) {
        int orig_i = shuffled.teacher_order[i];
        const auto &teacher_id = data.teachers[orig_i].id;
        
        // Use precomputed eligible courses
        auto it = precomp.teacher_eligible_courses.find(teacher_id);
        if (it != precomp.teacher_eligible_courses.end()) {
            for (int orig_j : it->second) {
                if (orig_j < J) { // Safety check
                    int j = orig_to_shuffled_course[orig_j];
                    eligible[i][j] = true;
                }
            }
        }
    }
    return eligible;
}

static TeacherAvailabilityMatrix build_teacher_available_slots(const ProblemData &data, const ShuffledIndices &shuffled) {
    int I = (int)data.teachers.size(), L = (int)data.classrooms.days.size(), M = (int)data.classrooms.periods.size();
    TeacherAvailabilityMatrix available(I, vector<vector<bool>>(L, vector<bool>(M, false)));
    int total_slots = L * M;
    
    for (int i = 0; i < I; ++i) {
        int orig_i = shuffled.teacher_order[i];
        int time_pref_count = (int)data.teachers[orig_i].time_pref.size();
        
        bool use_all_slots = (time_pref_count == 0) || (total_slots > 0 && time_pref_count < total_slots * 0.3);
        
        if (use_all_slots) {
            for (int l = 0; l < L; ++l) {
                for (int m = 0; m < M; ++m) {
                    available[i][l][m] = true;
                }
            }
        } else {
            for (const auto &tp : data.teachers[orig_i].time_pref) {
                auto day_it = find(data.classrooms.days.begin(), data.classrooms.days.end(), tp.day);
                auto period_it = find(data.classrooms.periods.begin(), data.classrooms.periods.end(), tp.period);
                if (day_it != data.classrooms.days.end() && period_it != data.classrooms.periods.end()) {
                    int l = day_it - data.classrooms.days.begin();
                    int m = period_it - data.classrooms.periods.begin();
                    available[i][l][m] = true;
                }
            }
        }
    }
    return available;
}

static void add_max_courses_constraint(CpModelBuilder &model, const ProblemData &data, const ShuffledIndices &shuffled,
                                       const EligibilityMatrix &eligible, const DecisionVariableMap &Y) {
    int I = (int)data.teachers.size(), J = (int)data.courses.size(), L = (int)data.classrooms.days.size(), M = (int)data.classrooms.periods.size();
    using SectionIndicatorKey = tuple<int, int, int>;
    map<SectionIndicatorKey, BoolVar> Z;
    
    for (int i = 0; i < I; ++i) {
        for (int j = 0; j < J; ++j) {
            if (!eligible[i][j]) continue;
            int orig_j = shuffled.course_order[j], S_j = (int)data.courses[orig_j].sections.size();
            for (int k = 0; k < S_j; ++k) {
                Z[{i, j, k}] = model.NewBoolVar();
                LinearExpr section_sum = 0;
                int r = data.courses[orig_j].sections[shuffled.section_orders[j][k]].required_periods;
                for (int l = 0; l < L; ++l) {
                    for (int m0 = 0; m0 + r <= M; ++m0) {
                        auto it = Y.find({i, j, k, l, m0});
                        if (it != Y.end()) section_sum += it->second;
                    }
                }
                model.AddEquality(Z[{i, j, k}], section_sum);
            }
        }
    }
    
    for (int i = 0; i < I; ++i) {
        int orig_i = shuffled.teacher_order[i];
        map<int, BoolVar> course_indicators;
        for (int j = 0; j < J; ++j) {
            if (!eligible[i][j]) continue;
            int orig_j = shuffled.course_order[j], S_j = (int)data.courses[orig_j].sections.size();
            BoolVar teaches_course = model.NewBoolVar();
            course_indicators[j] = teaches_course;
            LinearExpr section_sum = 0;
            for (int k = 0; k < S_j; ++k) {
                auto it = Z.find({i, j, k});
                if (it != Z.end()) {
                    model.AddGreaterOrEqual(teaches_course, it->second);
                    section_sum += it->second;
                }
            }
            if (S_j > 0) model.AddLessOrEqual(section_sum, teaches_course * S_j);
        }
        if (!course_indicators.empty()) {
            LinearExpr num_courses = 0;
            for (const auto &[j, indicator] : course_indicators) num_courses += indicator;
            model.AddLessOrEqual(num_courses, data.teachers[orig_i].max_courses);
        }
    }
}

static void add_min_max_teachers_per_course_constraint(CpModelBuilder &model, const ProblemData &data, const ShuffledIndices &shuffled,
                                                        const EligibilityMatrix &eligible, const DecisionVariableMap &Y, const vector<ViableSlot> &viable) {
    int I = (int)data.teachers.size(), J = (int)data.courses.size();
    
    for (int j = 0; j < J; ++j) {
        int orig_j = shuffled.course_order[j];
        int min_t = data.courses[orig_j].min_teachers;
        int max_t = data.courses[orig_j].max_teachers;
        
        // Count eligible teachers for this course
        int eligible_count = 0;
        for (int i = 0; i < I; ++i) {
            if (eligible[i][j]) eligible_count++;
        }
        
        // Only add constraint if min > 0 or max is meaningful (max > 0 and max < eligible_count)
        if (min_t > 0 || (max_t > 0 && max_t < eligible_count)) {
            map<int, BoolVar> teacher_indicator;
            
            // Create indicator variable for each eligible teacher teaching this course
            for (int i = 0; i < I; ++i) {
                if (!eligible[i][j]) continue;
                
                BoolVar indicator = model.NewBoolVar();
                teacher_indicator[i] = indicator;
                
                LinearExpr section_sum = 0;
                for (const auto &slot : viable) {
                    if (slot.i == i && slot.j == j) {
                        auto it = Y.find({slot.i, slot.j, slot.k, slot.l, slot.m0});
                        if (it != Y.end()) {
                            section_sum += it->second;
                        }
                    }
                }
                
                // Only need >= constraint: if teacher teaches any section, indicator = 1
                // The <= constraint is redundant and can make solver slower
                model.AddGreaterOrEqual(section_sum, indicator);
            }
            
            // Add min/max constraints on number of teachers
            if (!teacher_indicator.empty()) {
                LinearExpr total_teachers = 0;
                for (const auto &[i, indicator] : teacher_indicator) {
                    total_teachers += indicator;
                }
                
                if (min_t > 0) {
                    model.AddGreaterOrEqual(total_teachers, min_t);
                }
                if (max_t > 0 && max_t < eligible_count) {
                    model.AddLessOrEqual(total_teachers, max_t);
                }
            }
        }
    }
}

static void add_classroom_time_conflict_constraint(CpModelBuilder &model, const ProblemData &data, const ShuffledIndices &shuffled,
                                                     const DecisionVariableMap &Y, const vector<ViableSlot> &viable) {
    int L = (int)data.classrooms.days.size();
    int M = (int)data.classrooms.periods.size();
    int num_classrooms = (int)data.classrooms.classrooms.size();
    
    // Constraint: At most num_classrooms assignments can occur at any given timeslot
    // Build map: (day_l, period_m) -> LinearExpr of assignments using this timeslot
    map<pair<int, int>, LinearExpr> timeslot_usage;
    
    for (const auto &slot : viable) {
        auto it = Y.find({slot.i, slot.j, slot.k, slot.l, slot.m0});
        if (it == Y.end()) continue;
        
        // For each period this slot covers, count it as using a classroom
        for (int p = 0; p < slot.r; ++p) {
            int period_m = slot.m0 + p;
            if (period_m >= M) continue;
            
            auto key = make_pair(slot.l, period_m);
            timeslot_usage[key] += it->second;
        }
    }
    
    // Add constraints: at most num_classrooms assignments per timeslot
    for (const auto &[key, expr] : timeslot_usage) {
        model.AddLessOrEqual(expr, num_classrooms);
    }
}

static void add_soft_workload_objective(CpModelBuilder &model, const ProblemData &data, const ShuffledIndices &shuffled,
                                        const EligibilityMatrix &eligible, const DecisionVariableMap &Y,
                                        const vector<IntVar> &teacher_workload_vars, double soft_weight) {
    if (soft_weight <= 0.0 || teacher_workload_vars.empty()) return;
    int I = (int)data.teachers.size();
    int total_periods = compute_total_periods(data, shuffled);
    int avg_workload = (I > 0) ? (total_periods / I) : 0;
    int max_workload = 0;
    for (int i = 0; i < I; ++i) max_workload = max(max_workload, compute_max_possible_workload(data, shuffled, eligible, i));
    
    double adaptive_weight = soft_weight * (total_periods > 0 ? (1000.0 * I / total_periods) : 1000.0);
    int weight = (int)adaptive_weight;
    if (weight > 0) {
        IntVar max_workload_var = model.NewIntVar({0, max_workload});
        IntVar min_workload_var = model.NewIntVar({0, max_workload});
        
        for (int i = 0; i < I; ++i) {
            model.AddGreaterOrEqual(max_workload_var, teacher_workload_vars[i]);
            model.AddLessOrEqual(min_workload_var, teacher_workload_vars[i]);
        }
        
        // Minimize workload spread (max - min) as approximation
        IntVar workload_spread = model.NewIntVar({0, max_workload});
        model.AddEquality(workload_spread, max_workload_var - min_workload_var);
        model.Minimize(workload_spread * weight);
    }
}

static void build_cpsat_model(
    CpModelBuilder &model,
    const ProblemData &data,
    const PrecomputedData &precomp,
    const ShuffledIndices &shuffled,
    DecisionVariableMap &Y,
    vector<IntVar> &teacher_workload_vars,
    Phase2Mode mode = Phase2Mode::LIGHT_QUALITY) {
    
    double soft_weight = (mode == Phase2Mode::LIGHT_QUALITY) ? 0.01 : 0.0;
    int I = (int)data.teachers.size();
    int J = (int)data.courses.size();
    int L = (int)data.classrooms.days.size();
    int M = (int)data.classrooms.periods.size();
    
    EligibilityMatrix eligible = build_eligibility_matrix(data, precomp, shuffled);
    TeacherAvailabilityMatrix teacher_available = build_teacher_available_slots(data, shuffled);
    
    vector<ViableSlot> viable = precompute_viable_slots(data, precomp, shuffled, eligible, teacher_available);
    
    for (const auto &slot : viable) {
        Y[{slot.i, slot.j, slot.k, slot.l, slot.m0}] = model.NewBoolVar();
    }
    
    add_section_assignment_constraint(model, data, shuffled, eligible, Y, viable);
    add_teacher_time_conflict_constraint(model, data, shuffled, eligible, Y, viable);
    add_course_section_overlap_constraint(model, data, shuffled, eligible, Y, viable);
    add_workload_constraints(model, data, shuffled, eligible, Y, viable, teacher_workload_vars, soft_weight);
    add_max_courses_constraint(model, data, shuffled, eligible, Y);
    add_min_max_teachers_per_course_constraint(model, data, shuffled, eligible, Y, viable);
    add_soft_workload_objective(model, data, shuffled, eligible, Y, teacher_workload_vars, soft_weight);
}

static vector<InitialSolution> generate_solutions_with_collector(
    const ProblemData &data,
    const PrecomputedData &precomp,
    const ShuffledIndices &shuffled,
    int max_solutions,
    int random_seed,
    int time_limit_seconds = 10) {
    
    vector<InitialSolution> all_solutions;
    mt19937 rng(random_seed);
    
    CpModelBuilder model;
    DecisionVariableMap Y;
    vector<IntVar> teacher_workload_vars;
    build_cpsat_model(model, data, precomp, shuffled, Y, 
                      teacher_workload_vars, Phase2Mode::LIGHT_QUALITY);
    
    Model sat_model;
    configure_solver(sat_model, time_limit_seconds, random_seed, false);
    
    for (int sol_idx = 0; sol_idx < max_solutions; ++sol_idx) {
        auto response = SolveCpModel(model.Build(), &sat_model);
        
        if (!is_solution_feasible(response.status())) {
            break;
        }
        
        InitialSolution sol;
        extract_solution(response, Y, data, shuffled, sol);
        
        if (sol.assignments.empty() || !validate_solution_completeness(sol, data)) {
            break;
        }
        
        all_solutions.push_back(sol);
        
        if (sol_idx < max_solutions - 1) {
            auto section_assignments = collect_section_assignments(response, Y);
            add_no_good_cut(model, section_assignments, rng);
        }
    }
    
    return all_solutions;
}

static InitialSolution try_solve(
    const ProblemData &data,
    int random_seed,
    Phase2Mode mode,
    int time_limit_seconds) {
    
    PrecomputedData precomp = precompute_data(data);
    mt19937 rng = create_random_generator(random_seed);
    ShuffledIndices shuffled = create_shuffled_indices(data, precomp, rng);
    
    CpModelBuilder model;
    DecisionVariableMap Y;
    vector<IntVar> teacher_workload_vars;
    build_cpsat_model(model, data, precomp, shuffled, Y,
                      teacher_workload_vars, mode);
    
    return solve_and_extract_solution(model, data, shuffled, Y, 
                                      time_limit_seconds, random_seed, true);
}

InitialSolution construct_initial_solution(
    const ProblemData &data,
    int random_seed,
    bool shuffle_sections,
    bool shuffle_teachers,
    bool shuffle_timeslots) {
    
    PhaseLogger::set_phase("PHASE2");
    PhaseLogger::info("Constructing initial solution with " + 
                      std::to_string(data.teachers.size()) + " teachers, " +
                      std::to_string(data.courses.size()) + " courses");
    
    // Try FEASIBLE_ONLY mode first with shorter time limit
    PhaseLogger::debug("Attempting FEASIBLE_ONLY mode (time limit: 5s)");
    if (auto sol = try_solve(data, random_seed, Phase2Mode::FEASIBLE_ONLY, 5); !sol.assignments.empty()) {
        PhaseLogger::info("Initial solution found in FEASIBLE_ONLY mode with " + 
                         std::to_string(sol.assignments.size()) + " assignments");
        return sol;
    }
    
    // Fallback to LIGHT_QUALITY mode with longer time limit
    PhaseLogger::debug("FEASIBLE_ONLY failed, attempting LIGHT_QUALITY mode (time limit: 15s)");
    auto sol = try_solve(data, random_seed, Phase2Mode::LIGHT_QUALITY, 15);
    if (!sol.assignments.empty()) {
        PhaseLogger::info("Initial solution found in LIGHT_QUALITY mode with " + 
                         std::to_string(sol.assignments.size()) + " assignments");
    } else {
        PhaseLogger::warn("Failed to construct initial solution");
    }
    return sol;
}

vector<InitialSolution> generate_solution_pool(
    const ProblemData &data,
    int K,
    double min_diversity,
    int max_solutions) {
    
    PhaseLogger::set_phase("PHASE2");
    PhaseLogger::info("Generating solution pool: K=" + std::to_string(K) + 
                      ", max_solutions=" + std::to_string(max_solutions));
    
    SolutionPool pool;
    mt19937 rng = create_random_generator();
    PrecomputedData precomp = precompute_data(data);
    
    if (min_diversity < 0) {
        int total_sections = count_total_sections(data);
        min_diversity = compute_adaptive_diversity_threshold(total_sections);
        PhaseLogger::debug("Adaptive diversity threshold: " + std::to_string(min_diversity));
    }
    
    int attempts = 0;
    
    while (attempts < K && (int)pool.size() < max_solutions) {
        attempts++;
        int random_seed = random_int_in_range(rng, 1, 1000000);
        
        mt19937 attempt_rng(random_seed);
        ShuffledIndices shuffled = create_shuffled_indices(data, precomp, attempt_rng);
        Phase2Mode mode = (attempts < K / 2) ? Phase2Mode::FEASIBLE_ONLY : Phase2Mode::LIGHT_QUALITY;
        
        CpModelBuilder model;
        DecisionVariableMap Y;
        vector<IntVar> teacher_workload_vars;
        build_cpsat_model(model, data, precomp, shuffled, Y,
                          teacher_workload_vars, mode);
        
        InitialSolution sol = solve_and_extract_solution(model, data, shuffled, Y, 
                                                         10, random_seed, true);
        
        if (sol.assignments.empty()) {
            PhaseLogger::debug("Attempt " + std::to_string(attempts) + " failed to find solution");
            continue;
        }
        
        if (is_solution_diverse_enough(sol, pool, min_diversity)) {
            pool.push_back(sol);
            PhaseLogger::debug("Added solution " + std::to_string(pool.size()) + 
                              " to pool (diversity check passed)");
        } else {
            PhaseLogger::debug("Solution " + std::to_string(attempts) + 
                              " rejected (insufficient diversity)");
        }
    }
    
    PhaseLogger::info("Solution pool generation completed: " + 
                     std::to_string(pool.size()) + " solutions found");
    return pool;
}

vector<InitialSolution> select_diverse_solutions(
    const SolutionPool &pool,
    int N) {
    if (pool.empty() || N <= 0)
        return {};
    
    if ((int)pool.size() <= N)
        return pool;
    
    vector<InitialSolution> selected;
    vector<bool> used(pool.size(), false);
    
    mt19937 rng = create_random_generator();
    int first_idx = random_int_in_range(rng, 0, (int)pool.size() - 1);
    selected.push_back(pool[first_idx]);
    used[first_idx] = true;
    
    for (int i = 1; i < N && (int)selected.size() < N; ++i) {
        int best_idx = -1;
        double best_score = -1.0;
        
        for (size_t j = 0; j < pool.size(); ++j) {
            if (used[j]) continue;
            
            double total_diversity = 0.0;
            for (const auto &sel : selected) {
                total_diversity += compute_diversity(pool[j], sel);
            }
            
            if (total_diversity > best_score) {
                best_score = total_diversity;
                best_idx = j;
            }
        }
        
        if (best_idx >= 0) {
            selected.push_back(pool[best_idx]);
            used[best_idx] = true;
        }
    }
    
    return selected;
}

double evaluate_phase2_solution_quick(const InitialSolution &sol, const ProblemData &data) {
    if (sol.assignments.empty()) return 0.0;
    PrecomputedData precomp = precompute_data(data);
    TeacherWorkloadMap teacher_periods;
    TimeslotSet used_timeslots;
    
    for (const auto &a : sol.assignments) {
        teacher_periods[a.teacher_id] += get_required_periods_fast(precomp, a.course_id, a.section_id);
        used_timeslots.insert(::format_timeslot_key(a.day, a.period));
    }
    
    if (teacher_periods.empty()) return 0.0;
    vector<int> workloads;
    for (const auto &tp : teacher_periods) workloads.push_back(tp.second);
    
    double max_workload = *max_element(workloads.begin(), workloads.end());
    double workload_score = (max_workload > 0) ? max(0.0, 1.0 - (compute_stddev(workloads) / max_workload)) : 1.0;
    
    int total_timeslots = data.classrooms.days.size() * data.classrooms.periods.size();
    if (total_timeslots == 0) return 0.0;
    double utilization = (double)used_timeslots.size() / total_timeslots;
    double timeslot_score = (utilization < 0.3) ? utilization / 0.3 : (utilization <= 0.7) ? 1.0 : 1.0 - (utilization - 0.7) / 0.3;
    
    return 0.6 * workload_score + 0.4 * clamp_value(timeslot_score, 0.0, 1.0);
}

bool is_phase2_solution_good_enough(
    const InitialSolution &sol,
    const ProblemData &data,
    double min_score) {
    double score = evaluate_phase2_solution_quick(sol, data);
    return score >= min_score;
}
