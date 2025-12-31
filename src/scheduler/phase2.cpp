#include "phase2.h"
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
#include <cmath>
#include <sstream>
#include <numeric>

using namespace operations_research;
using namespace operations_research::sat;
using namespace std;

// ============================================================================
// TYPE DEFINITIONS
// ============================================================================

using DecisionVariableKey = tuple<int, int, int, int, int>;

// Hash function for DecisionVariableKey tuple
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
using TeacherAvailabilitySlots = vector<unordered_set<string>>;
using TeacherWorkloadMap = unordered_map<string, int>;
using TimeslotSet = unordered_set<string>;
using AssignmentMapping = map<pair<string, string>, pair<string, pair<string, string>>>;
using SolutionPool = vector<InitialSolution>;

// Precomputed lookup structures for O(1) access
struct PrecomputedData {
    // course_id + section_id -> required_seats
    map<pair<string, string>, int> section_seats;
    // course_id + section_id -> required_periods
    map<pair<string, string>, int> section_periods;
    // teacher_id -> set of eligible course indices
    unordered_map<string, vector<int>> teacher_eligible_courses;
    // classroom_id -> capacity
    unordered_map<string, int> classroom_capacity;
    // course index -> course_id
    vector<string> course_id_map;
    // section index -> section_id (nested: course_idx -> section_idx -> section_id)
    vector<vector<string>> section_id_map;
};

// Shuffled index mappings for diversity
struct ShuffledIndices {
    vector<int> teacher_order;
    vector<int> course_order;
    vector<int> day_order;
    vector<int> period_order;
    vector<vector<int>> section_orders; // per course
};

// ============================================================================
// HELPER FUNCTIONS - Data Access & Precomputation
// ============================================================================

static string format_timeslot_key(const string &day, const string &period) {
    return day + "|" + period;
}

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

// Precompute all lookup structures for O(1) access
static PrecomputedData precompute_data(const ProblemData &data) {
    PrecomputedData precomp;
    
    // Precompute section seats and periods
    for (const auto &course : data.courses) {
        for (const auto &section : course.sections) {
            precomp.section_seats[{course.id, section.id}] = section.required_seats;
            precomp.section_periods[{course.id, section.id}] = section.required_periods;
        }
    }
    
    // Precompute teacher eligible courses (by index)
    for (size_t i = 0; i < data.teachers.size(); ++i) {
        const auto &teacher = data.teachers[i];
        for (size_t j = 0; j < data.courses.size(); ++j) {
            const auto &course = data.courses[j];
            if (find(teacher.eligible_courses.begin(), teacher.eligible_courses.end(), 
                     course.id) != teacher.eligible_courses.end()) {
                precomp.teacher_eligible_courses[teacher.id].push_back(j);
            }
        }
    }
    
    // Precompute classroom capacities
    for (const auto &room : data.classrooms.classrooms) {
        precomp.classroom_capacity[room.id] = room.capacity;
    }
    
    // Precompute course and section ID maps
    for (const auto &course : data.courses) {
        precomp.course_id_map.push_back(course.id);
        vector<string> section_ids;
        for (const auto &section : course.sections) {
            section_ids.push_back(section.id);
        }
        precomp.section_id_map.push_back(section_ids);
    }
    
    return precomp;
}

// Get required seats using precomputed map
static int get_required_seats_fast(const PrecomputedData &precomp, 
                                    const string &course_id, 
                                    const string &section_id) {
    auto it = precomp.section_seats.find({course_id, section_id});
    return (it != precomp.section_seats.end()) ? it->second : 0;
}

// Get required periods using precomputed map
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

// ============================================================================
// HELPER FUNCTIONS - Statistics & Evaluation
// ============================================================================

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

// ============================================================================
// HELPER FUNCTIONS - Shuffling for Diversity & Ordering Heuristics
// ============================================================================

// Count number of eligible teachers for a section
static int count_eligible_teachers_for_section(
    const ProblemData &data,
    int course_idx,
    int section_idx) {
    int count = 0;
    const auto &course = data.courses[course_idx];
    const string &course_id = course.id;
    
    for (const auto &teacher : data.teachers) {
        if (find(teacher.eligible_courses.begin(),
                 teacher.eligible_courses.end(),
                 course_id) != teacher.eligible_courses.end()) {
            count++;
        }
    }
    return count;
}

static ShuffledIndices create_shuffled_indices(const ProblemData &data, mt19937 &rng) {
    ShuffledIndices shuffled;
    
    // Shuffle teacher order
    shuffled.teacher_order.resize(data.teachers.size());
    iota(shuffled.teacher_order.begin(), shuffled.teacher_order.end(), 0);
    shuffle(shuffled.teacher_order.begin(), shuffled.teacher_order.end(), rng);
    
    // Shuffle course order
    shuffled.course_order.resize(data.courses.size());
    iota(shuffled.course_order.begin(), shuffled.course_order.end(), 0);
    shuffle(shuffled.course_order.begin(), shuffled.course_order.end(), rng);
    
    // Shuffle day order
    shuffled.day_order.resize(data.classrooms.days.size());
    iota(shuffled.day_order.begin(), shuffled.day_order.end(), 0);
    shuffle(shuffled.day_order.begin(), shuffled.day_order.end(), rng);
    
    // Shuffle period order
    shuffled.period_order.resize(data.classrooms.periods.size());
    iota(shuffled.period_order.begin(), shuffled.period_order.end(), 0);
    shuffle(shuffled.period_order.begin(), shuffled.period_order.end(), rng);
    
    // Order sections by difficulty heuristic (NOT shuffle)
    // Priority: required_periods DESC, eligible_teachers ASC
    // This helps CP-SAT find feasible solutions faster by handling hard sections first
    shuffled.section_orders.resize(data.courses.size());
    for (int j = 0; j < (int)data.courses.size(); ++j) {
        // j is shuffled course index
        int orig_j = shuffled.course_order[j];  // Get original course index
        const auto &course = data.courses[orig_j];
        int num_sections = (int)course.sections.size();
        
        // Create vector of section indices with their difficulty scores
        vector<pair<int, pair<int, int>>> section_scores; // (section_idx, (required_periods, eligible_teachers))
        
        for (int k = 0; k < num_sections; ++k) {
            int required_periods = course.sections[k].required_periods;
            int eligible_teachers = count_eligible_teachers_for_section(data, orig_j, k);
            // Store as (required_periods, -eligible_teachers) for DESC, ASC ordering
            section_scores.push_back({k, {required_periods, eligible_teachers}});
        }
        
        // Sort: required_periods DESC, then eligible_teachers ASC
        sort(section_scores.begin(), section_scores.end(),
             [](const pair<int, pair<int, int>> &a, const pair<int, pair<int, int>> &b) {
                 if (a.second.first != b.second.first) {
                     return a.second.first > b.second.first; // DESC: longer sections first
                 }
                 return a.second.second < b.second.second; // ASC: fewer eligible teachers first
             });
        
        // Extract ordered section indices
        shuffled.section_orders[j].resize(num_sections);
        for (int k = 0; k < num_sections; ++k) {
            shuffled.section_orders[j][k] = section_scores[k].first;
        }
    }
    
    return shuffled;
}

// ============================================================================
// HELPER FUNCTIONS - Solution Extraction & Diversity
// ============================================================================

static void extract_solution(const CpSolverResponse &response,
                             const DecisionVariableMap &Y,
                             const ProblemData &data,
                             const ShuffledIndices &shuffled,
                             InitialSolution &sol) {
    for (const auto &kv : Y) {
        if (SolutionBooleanValue(response, kv.second)) {
            int i, j, k, l, m0;
            tie(i, j, k, l, m0) = kv.first;
            
            // Map back from shuffled indices to original
            int orig_i = shuffled.teacher_order[i];
            int orig_j = shuffled.course_order[j];
            // k is shuffled section index in shuffled course j
            // shuffled.section_orders[j][k] maps it to original section index in original course orig_j
            int orig_k = shuffled.section_orders[j][k];
            int orig_l = shuffled.day_order[l];
            int orig_m0 = shuffled.period_order[m0];
            
            InitialSolution::Assignment a;
            a.teacher_id = data.teachers[orig_i].id;
            a.course_id = data.courses[orig_j].id;
            a.section_id = data.courses[orig_j].sections[orig_k].id;
            a.day = data.classrooms.days[orig_l];
            a.period = data.classrooms.periods[orig_m0];
            a.classroom_id = "";
            
            a.initial_teacher = a.teacher_id;
            a.initial_timeslot = format_timeslot_key(a.day, a.period);
            
            sol.assignments.push_back(a);
        }
    }
}

static double compute_diversity(const InitialSolution &sol1, const InitialSolution &sol2) {
    if (sol1.assignments.size() != sol2.assignments.size())
        return 1.0;
    
    if (sol1.assignments.empty())
        return 0.0;
    
    AssignmentMapping map1, map2;
    
    for (const auto &a : sol1.assignments)
        map1[{a.course_id, a.section_id}] = {a.teacher_id, {a.day, a.period}};
    
    for (const auto &a : sol2.assignments)
        map2[{a.course_id, a.section_id}] = {a.teacher_id, {a.day, a.period}};
    
    int total = 0;
    int teacher_diff = 0;
    int timeslot_diff = 0;
    
    for (const auto &kv : map1) {
        auto it = map2.find(kv.first);
        if (it == map2.end()) {
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
    
    if (total == 0) return 0.0;
    
    double teacher_diversity = (double)teacher_diff / total;
    double timeslot_diversity = (double)timeslot_diff / total;
    
    return 0.5 * teacher_diversity + 0.5 * timeslot_diversity;
}

static bool is_solution_diverse_enough(const InitialSolution &sol, 
                                       const SolutionPool &pool,
                                       double min_diversity = 0.1) {
    for (const auto &prev : pool) {
        double diversity = compute_diversity(sol, prev);
        if (diversity < min_diversity)
            return false;
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

// ============================================================================
// CP-SAT MODEL BUILDING - Core Construction
// ============================================================================

// Build CP-SAT model with hard constraints and soft objectives
static void build_cpsat_model(
    CpModelBuilder &model,
    const ProblemData &data,
    const PrecomputedData &precomp,
    const ShuffledIndices &shuffled,
    DecisionVariableMap &Y,
    vector<IntVar> &teacher_workload_vars,
    double soft_weight = 0.01) {
    
    int I = (int)data.teachers.size();
    int J = (int)data.courses.size();
    int L = (int)data.classrooms.days.size();
    int M = (int)data.classrooms.periods.size();
    
    // Build eligibility matrix (using shuffled indices)
    EligibilityMatrix eligible(I, vector<bool>(J, false));
    for (int i = 0; i < I; ++i) {
        int orig_i = shuffled.teacher_order[i];
        const auto &teacher = data.teachers[orig_i];
        for (int j = 0; j < J; ++j) {
            int orig_j = shuffled.course_order[j];
            const auto &course = data.courses[orig_j];
            eligible[i][j] = (find(teacher.eligible_courses.begin(),
                                   teacher.eligible_courses.end(),
                                   course.id) != teacher.eligible_courses.end());
        }
    }
    
    // Phase 2: Do NOT filter by teacher availability (time_pref)
    // Time preferences are soft constraints handled in Phase 3
    // This allows Phase 2 to find feasible solutions more easily
    
    // Create decision variables with domain reduction
    // Only create variables for feasible assignments
    for (int i = 0; i < I; ++i) {
        int orig_i = shuffled.teacher_order[i];
        for (int j = 0; j < J; ++j) {
            if (!eligible[i][j]) continue;
            
            int orig_j = shuffled.course_order[j];
            int S_j = (int)data.courses[orig_j].sections.size();
            
            for (int k = 0; k < S_j; ++k) {
                int orig_k = shuffled.section_orders[j][k];
                int r = data.courses[orig_j].sections[orig_k].required_periods;
                int seats = data.courses[orig_j].sections[orig_k].required_seats;
                
                // Check if any classroom has sufficient capacity
                bool has_feasible_room = false;
                for (const auto &room : data.classrooms.classrooms) {
                    if (room.capacity >= seats) {
                        has_feasible_room = true;
                        break;
                    }
                }
                if (!has_feasible_room) continue; // Skip if no feasible room
                
                for (int l = 0; l < L; ++l) {
                    int orig_l = shuffled.day_order[l];
                    for (int m0 = 0; m0 + r <= M; ++m0) {
                        int orig_m0 = shuffled.period_order[m0];
                        
                        // Phase 2: Do NOT filter by time_pref availability
                        // Time preferences are soft constraints handled in Phase 3
                        // This allows Phase 2 to find feasible solutions more easily
                        
                        Y[{i, j, k, l, m0}] = model.NewBoolVar();
                    }
                }
            }
        }
    }
    
    // Hard constraint: Each section assigned exactly once
    for (int j = 0; j < J; ++j) {
        int orig_j = shuffled.course_order[j];
        int S_j = (int)data.courses[orig_j].sections.size();
        
        for (int k = 0; k < S_j; ++k) {
            LinearExpr sum = 0;
            for (int i = 0; i < I; ++i) {
                if (!eligible[i][j]) continue;
                for (int l = 0; l < L; ++l) {
                    for (int m0 = 0; m0 < M; ++m0) {
                        auto it = Y.find({i, j, k, l, m0});
                        if (it != Y.end())
                            sum += it->second;
                    }
                }
            }
            model.AddEquality(sum, 1);
        }
    }
    
    // Hard constraint: No overlapping assignments per teacher
    // REMOVED: This constraint was too strong and prevented finding feasible solutions
    // Full overlap checking is handled in Phase 3 (SA/Tabu)
    
    // Soft constraint: Balance teacher workload (minimize stddev)
    // Create workload variables for each teacher
    teacher_workload_vars.clear();
    teacher_workload_vars.resize(I);
    
    for (int i = 0; i < I; ++i) {
        LinearExpr workload = 0;
        for (int j = 0; j < J; ++j) {
            if (!eligible[i][j]) continue;
            
            int orig_j = shuffled.course_order[j];
            int S_j = (int)data.courses[orig_j].sections.size();
            
            for (int k = 0; k < S_j; ++k) {
                int orig_k = shuffled.section_orders[j][k];
                int r = data.courses[orig_j].sections[orig_k].required_periods;
                
                for (int l = 0; l < L; ++l) {
                    for (int m0 = 0; m0 + r <= M; ++m0) {
                        auto it = Y.find({i, j, k, l, m0});
                        if (it != Y.end()) {
                            workload += r * it->second;
                        }
                    }
                }
            }
        }
        
        // Create bounded workload variable
        int max_possible_workload = 0;
        for (int j = 0; j < J; ++j) {
            if (!eligible[i][j]) continue;
            int orig_j = shuffled.course_order[j];
            for (const auto &section : data.courses[orig_j].sections) {
                max_possible_workload += section.required_periods;
            }
        }
        teacher_workload_vars[i] = model.NewIntVar({0, max_possible_workload});
        model.AddEquality(teacher_workload_vars[i], workload);
    }
    
    // Phase 2: Remove timeslot utilization constraint
    // It causes conflicts and is not needed for feasibility
    // Timeslot optimization is handled in Phase 3
    
    // Add soft objectives with small weights
    // CRITICAL: CP-SAT does NOT add objectives - each Minimize() overwrites the previous one
    // Must combine all objectives into a single LinearExpr
    if (soft_weight > 0.0 && !teacher_workload_vars.empty()) {
        // Calculate accurate bounds for workload
        int total_periods = 0;
        int max_section_length = 0;
        for (int j = 0; j < J; ++j) {
            int orig_j = shuffled.course_order[j];
            for (const auto &section : data.courses[orig_j].sections) {
                total_periods += section.required_periods;
                max_section_length = max(max_section_length, section.required_periods);
            }
        }
        
        int avg_workload = (I > 0) ? (total_periods / I) : 0;
        int slack = avg_workload + max_section_length;
        int upper_bound = max(slack, max_section_length); // Ensure at least max_section_length
        
        // Minimize workload variance (approximate by minimizing max - min)
        IntVar max_workload = model.NewIntVar({0, upper_bound});
        IntVar min_workload = model.NewIntVar({0, upper_bound});
        model.AddMaxEquality(max_workload, teacher_workload_vars);
        model.AddMinEquality(min_workload, teacher_workload_vars);
        IntVar workload_range = model.NewIntVar({0, upper_bound});
        model.AddEquality(workload_range, max_workload - min_workload);
        
        // Combine all objectives into a single LinearExpr
        // CRITICAL: CP-SAT does NOT add objectives - each Minimize() overwrites the previous one
        LinearExpr objective = 0;
        int weight = (int)(soft_weight * 1000);
        if (weight > 0) {
            objective += workload_range * weight;
        }
        
        // Only call Minimize() ONCE with the combined objective
        if (weight > 0) {
            model.Minimize(objective);
        }
    }
}

// ============================================================================
// SOLUTION POOL GENERATION - Using Iterative Solve with No-Good Cuts
// ============================================================================

// Generate multiple solutions using iterative solve with no-good cuts
// This collects multiple diverse solutions by solving iteratively and adding
// constraints to exclude previously found solutions
static vector<InitialSolution> generate_solutions_with_collector(
    const ProblemData &data,
    const PrecomputedData &precomp,
    const ShuffledIndices &shuffled,
    int max_solutions,
    int random_seed,
    int time_limit_seconds = 10) {
    
    vector<InitialSolution> all_solutions;
    
    // Build initial model
    CpModelBuilder model;
    DecisionVariableMap Y;
    vector<IntVar> teacher_workload_vars;
    build_cpsat_model(model, data, precomp, shuffled, Y, 
                      teacher_workload_vars, 0.01);
    
    Model sat_model;
    ostringstream param_stream;
    param_stream << "max_time_in_seconds:" << time_limit_seconds << " "
                 << "num_search_workers:8 "
                 << "cp_model_presolve:true "
                 << "linearization_level:0 "
                 << "random_seed:" << random_seed;
    sat_model.Add(NewSatParameters(param_stream.str()));
    
    // Collect solutions iteratively with no-good cuts
    for (int sol_idx = 0; sol_idx < max_solutions; ++sol_idx) {
        // Solve
        auto response = SolveCpModel(model.Build(), &sat_model);
        
        // Check if solution found
        if (response.status() != CpSolverStatus::FEASIBLE && 
            response.status() != CpSolverStatus::OPTIMAL) {
            break; // No more solutions
        }
        
        // Extract solution
        InitialSolution sol;
        extract_solution(response, Y, data, shuffled, sol);
        
        if (sol.assignments.empty()) {
            break;
        }
        
        all_solutions.push_back(sol);
        
        // Add no-good cut: sum(Y != last_solution) >= 1
        // This ensures the next solution is different
        // We add the constraint to the model builder for the next iteration
        if (sol_idx < max_solutions - 1) {
            LinearExpr diff_sum = 0;
            for (const auto &kv : Y) {
                bool is_set = SolutionBooleanValue(response, kv.second);
                if (is_set) {
                    // If variable was true in this solution, add (1 - Y) to force it to be false
                    diff_sum += (1 - kv.second);
                } else {
                    // If variable was false, add Y to force it to be true
                    diff_sum += kv.second;
                }
            }
            // At least one variable must differ from current solution
            model.AddGreaterOrEqual(diff_sum, 1);
        }
    }
    
    return all_solutions;
}

// ============================================================================
// MAIN PHASE2 FUNCTIONS
// ============================================================================

InitialSolution construct_initial_solution(
    const ProblemData &data,
    int random_seed,
    bool shuffle_sections,
    bool shuffle_teachers,
    bool shuffle_timeslots) {
    
    // Precompute lookup structures
    PrecomputedData precomp = precompute_data(data);
    
    // Create shuffled indices for diversity
    mt19937 rng = create_random_generator(random_seed);
    ShuffledIndices shuffled = create_shuffled_indices(data, rng);
    
    // Build CP-SAT model
    CpModelBuilder model;
    DecisionVariableMap Y;
    vector<IntVar> teacher_workload_vars;
    vector<IntVar> timeslot_usage_vars;
    
    // Build with soft constraints (lightweight)
    build_cpsat_model(model, data, precomp, shuffled, Y,
                      teacher_workload_vars, 0.01);
    
    // Configure solver
    Model sat_model;
    ostringstream param_stream;
    param_stream << "max_time_in_seconds:10 "
                 << "num_search_workers:8 "
                 << "cp_model_presolve:true "
                 << "linearization_level:0 "
                 << "stop_after_first_solution:true "
                 << "random_seed:" << random_seed;
    sat_model.Add(NewSatParameters(param_stream.str()));
    
    auto response = SolveCpModel(model.Build(), &sat_model);
    
    cout << "[Phase2] Status: " << CpSolverStatus_Name(response.status()) << endl;
    
    if (response.status() == CpSolverStatus::FEASIBLE || 
        response.status() == CpSolverStatus::OPTIMAL) {
        InitialSolution sol;
        extract_solution(response, Y, data, shuffled, sol);
        cout << "[Phase2] Found feasible solution (seed=" << random_seed 
             << ", leaving optimization space for Phase 3)" << endl;
        return sol;
    }
    
    cout << "[Phase2] No feasible solution found (seed=" << random_seed << ").\n";
    return InitialSolution();
}

vector<InitialSolution> generate_solution_pool(
    const ProblemData &data,
    int K,
    double min_diversity,
    int max_solutions) {
    
    SolutionPool pool;
    mt19937 rng = create_random_generator();
    
    // Precompute data once
    PrecomputedData precomp = precompute_data(data);
    
    if (min_diversity < 0) {
        int total_sections = count_total_sections(data);
        min_diversity = compute_adaptive_diversity_threshold(total_sections);
    }
    
    cout << "[Phase2] Generating solution pool (K=" << K 
         << ", min_diversity=" << min_diversity 
         << ", max_solutions=" << max_solutions << ")\n";
    
    int attempts = 0;
    int found = 0;
    
    while (attempts < K && (int)pool.size() < max_solutions) {
        attempts++;
        int random_seed = random_int_in_range(rng, 1, 1000000);
        
        cout << "[Phase2] Attempt " << attempts << "/" << K 
             << " (seed=" << random_seed << ")... ";
        
        // Create new shuffled indices for each attempt
        mt19937 attempt_rng(random_seed);
        ShuffledIndices shuffled = create_shuffled_indices(data, attempt_rng);
        
        // Adaptive soft_weight: disable workload soft constraint when difficult
        // First half of attempts: brute feasible (soft_weight = 0.0)
        // Second half: quality optimization (soft_weight = 0.01)
        double soft_weight = (attempts < K / 2) ? 0.0 : 0.01;
        
        // Build and solve model
        CpModelBuilder model;
        DecisionVariableMap Y;
        vector<IntVar> teacher_workload_vars;
        build_cpsat_model(model, data, precomp, shuffled, Y,
                          teacher_workload_vars, soft_weight);
        
        Model sat_model;
        ostringstream param_stream;
        param_stream << "max_time_in_seconds:10 "
                     << "num_search_workers:8 "
                     << "cp_model_presolve:true "
                     << "linearization_level:0 "
                     << "stop_after_first_solution:true "
                     << "random_seed:" << random_seed;
        sat_model.Add(NewSatParameters(param_stream.str()));
        
        auto response = SolveCpModel(model.Build(), &sat_model);
        
        if (response.status() != CpSolverStatus::FEASIBLE && 
            response.status() != CpSolverStatus::OPTIMAL) {
            cout << "No feasible solution\n";
            continue;
        }
        
        InitialSolution sol;
        extract_solution(response, Y, data, shuffled, sol);
        
        if (sol.assignments.empty()) {
            cout << "No feasible solution\n";
            continue;
        }
        
        found++;
        
        if (is_solution_diverse_enough(sol, pool, min_diversity)) {
            pool.push_back(sol);
            cout << "Added to pool (diversity OK, pool_size=" << pool.size() << ")\n";
        } else {
            cout << "Rejected (too similar to existing solutions)\n";
        }
    }
    
    cout << "[Phase2] Pool generation complete: " << found << " feasible found, "
         << pool.size() << " diverse solutions in pool\n";
    
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

InitialSolution assign_rooms_phase2(const ProblemData &data, const InitialSolution &initial) {
    InitialSolution result = initial;
    mt19937 rng = create_random_generator();
    
    // Precompute lookup for efficiency
    PrecomputedData precomp = precompute_data(data);
    
    for (auto &a : result.assignments) {
        if (a.classroom_id.empty()) {
            int seats = get_required_seats_fast(precomp, a.course_id, a.section_id);
            
            vector<string> feasible_rooms;
            for (const auto &room : data.classrooms.classrooms) {
                if (room.capacity >= seats) {
                    feasible_rooms.push_back(room.id);
                }
            }
            
            if (!feasible_rooms.empty()) {
                int random_idx = random_int_in_range(rng, 0, (int)feasible_rooms.size() - 1);
                a.classroom_id = feasible_rooms[random_idx];
            }
        }
    }
    return result;
}

double evaluate_phase2_solution_quick(
    const InitialSolution &sol,
    const ProblemData &data) {
    if (sol.assignments.empty())
        return 0.0;
    
    // Precompute lookup for efficiency
    PrecomputedData precomp = precompute_data(data);
    
    TeacherWorkloadMap teacher_periods;
    
    for (const auto &a : sol.assignments) {
        int required_periods = get_required_periods_fast(precomp, a.course_id, a.section_id);
        teacher_periods[a.teacher_id] += required_periods;
    }
    
    if (teacher_periods.empty())
        return 0.0;
    
    vector<int> workloads;
    for (const auto &tp : teacher_periods)
        workloads.push_back(tp.second);
    
    double workload_stddev = compute_stddev(workloads);
    double max_workload = *max_element(workloads.begin(), workloads.end());
    
    double workload_score = 0.0;
    if (max_workload > 0)
        workload_score = max(0.0, 1.0 - (workload_stddev / max_workload));
    else
        workload_score = 1.0;
    
    TimeslotSet used_timeslots;
    for (const auto &a : sol.assignments) {
        string timeslot = format_timeslot_key(a.day, a.period);
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
    
    timeslot_score = clamp_value(timeslot_score, 0.0, 1.0);
    
    double final_score = 0.6 * workload_score + 0.4 * timeslot_score;
    
    return final_score;
}

bool is_phase2_solution_good_enough(
    const InitialSolution &sol,
    const ProblemData &data,
    double min_score) {
    double score = evaluate_phase2_solution_quick(sol, data);
    return score >= min_score;
}
