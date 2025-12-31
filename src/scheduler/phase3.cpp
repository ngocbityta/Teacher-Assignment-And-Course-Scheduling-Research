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
#include <tuple>
#include <bitset>
#include <functional>

using namespace std;

namespace
{
    // ============================================================================
    // TYPE DEFINITIONS - Core Data Structures
    // ============================================================================
    
    // Slot: Represents a time slot (day + period) in the schedule
    struct Slot {
        int day_idx;      // Index of the day
        int period_idx;   // Index of the period within the day
    };
    
    // AssignmentChange: Tracks a change to an assignment during move evaluation
    struct AssignmentChange {
        int idx;                              // Index of the assignment being changed
        OptimalSolution::Assignment old_a;     // Original assignment before change
        OptimalSolution::Assignment new_a;    // New assignment after change
    };
    
    // Block: Represents a block of consecutive periods for chain moves
    struct Block {
        int assignment_idx;    // Index of the assignment in the solution
        string period;         // Starting period of the block
        int required_periods;  // Number of consecutive periods required
    };
    
    // ============================================================================
    // TYPE DEFINITIONS - Move Operations
    // ============================================================================
    
    // Move: Represents a move operation in the search space
    struct Move {
        enum Type {
            SINGLE_CHANGE,    // Change teacher or timeslot for one assignment
            BLOCK_RELOCATE,   // Relocate an assignment to a new timeslot
            CHAIN_MOVE        // Rotate a chain of consecutive assignments
        };
        Type type;                    // Type of move
        vector<int> indices;          // Indices of assignments affected by the move
        vector<Block> chain;          // Chain blocks (for CHAIN_MOVE type)
    };
    
    // MoveSpec: Specification for generating a move
    struct MoveSpec {
        Move::Type type = Move::SINGLE_CHANGE;  // Type of move to generate
        vector<int> assignment_indices;          // Assignment indices to modify
        string new_teacher_id;                    // New teacher ID (for teacher change)
        string new_day;                          // New day (for timeslot change)
        string new_period;                        // New period (for timeslot change)
        vector<Block> chain;                      // Chain blocks (for CHAIN_MOVE)
    };
    
    // SolIndex: Fast lookup index for solution constraints (defined here for use in MoveContext)
    struct SolIndex {
        unordered_map<string, unordered_set<int>> teacher_busy_idx;           // Teacher busy slots
        unordered_map<string, unordered_map<int, string>> course_slot_section_idx; // Course-section per slot
        unordered_map<string, unordered_set<string>> course_teachers;        // Teachers per course
        unordered_map<string, unordered_set<string>> course_teacher_sections; // Sections per course-teacher
        unordered_map<string, unordered_set<int>> classroom_busy_idx;         // Classroom busy slots
        int num_periods = 0;  // Number of periods per day
    };
    
    // MoveContext: Complete context for evaluating a move
    struct MoveContext {
        bool ok;                          // Whether the move is feasible
        Move move;                        // The move being evaluated
        OptimalSolution candidate;       // Candidate solution after applying move
        SolIndex idx_after;              // Solution index after move
        vector<AssignmentChange> changes; // List of assignment changes in this move
    };
    
    // MoveDelta: Delta values for evaluating move impact
    struct MoveDelta {
        int delta_hard = 0;              // Hard constraint violations (currently unused)
        double delta_soft_local = 0.0;   // Local soft constraint delta (preferences, stability)
        double delta_workload_var = 0.0;  // Change in workload variance
        double delta_compactness = 0.0;   // Change in compactness penalty
    };
    
    // ============================================================================
    // TYPE DEFINITIONS - Indexing and Delta Tracking
    // ============================================================================
    
    // SlotIndex: Integer index for a time slot (day_idx * num_periods + period_idx)
    using SlotIndex = int;
    
    // DeltaMode: Mode for building index deltas
    enum class DeltaMode {
        ADD,    // Add assignment to index
        REMOVE, // Remove assignment from index
        DIFF    // Compute difference (remove old, add new)
    };
    
    // IndexDelta: Tracks changes to solution indices for efficient updates
    struct IndexDelta {
        // Teacher busy slots
        vector<pair<string, SlotIndex>> teacher_add_idx;
        vector<pair<string, SlotIndex>> teacher_remove_idx;
        
        // Classroom busy slots
        vector<pair<string, SlotIndex>> classroom_add_idx;
        vector<pair<string, SlotIndex>> classroom_remove_idx;
        
        // Course-section assignments per slot
        vector<tuple<string, SlotIndex, string>> course_slot_section_add_idx;
        vector<tuple<string, SlotIndex, string>> course_slot_section_remove_idx;
        
        // Course-teacher assignments
        vector<pair<string, string>> course_teacher_add;
        vector<pair<string, string>> course_teacher_remove;
        
        // Course-teacher-section assignments
        vector<tuple<string, string, string>> course_teacher_section_add;
        vector<tuple<string, string, string>> course_teacher_section_remove;
        
        IndexDelta& operator+=(const IndexDelta& other) {
            teacher_add_idx.insert(teacher_add_idx.end(), other.teacher_add_idx.begin(), other.teacher_add_idx.end());
            teacher_remove_idx.insert(teacher_remove_idx.end(), other.teacher_remove_idx.begin(), other.teacher_remove_idx.end());
            classroom_add_idx.insert(classroom_add_idx.end(), other.classroom_add_idx.begin(), other.classroom_add_idx.end());
            classroom_remove_idx.insert(classroom_remove_idx.end(), other.classroom_remove_idx.begin(), other.classroom_remove_idx.end());
            course_slot_section_add_idx.insert(course_slot_section_add_idx.end(), other.course_slot_section_add_idx.begin(), other.course_slot_section_add_idx.end());
            course_slot_section_remove_idx.insert(course_slot_section_remove_idx.end(), other.course_slot_section_remove_idx.begin(), other.course_slot_section_remove_idx.end());
            course_teacher_add.insert(course_teacher_add.end(), other.course_teacher_add.begin(), other.course_teacher_add.end());
            course_teacher_remove.insert(course_teacher_remove.end(), other.course_teacher_remove.begin(), other.course_teacher_remove.end());
            course_teacher_section_add.insert(course_teacher_section_add.end(), other.course_teacher_section_add.begin(), other.course_teacher_section_add.end());
            course_teacher_section_remove.insert(course_teacher_section_remove.end(), other.course_teacher_section_remove.begin(), other.course_teacher_section_remove.end());
            return *this;
        }
    };
    
    
    // ============================================================================
    // CONSTANTS
    // ============================================================================
    
    constexpr int MAX_PERIOD_INDEX = 128;  // Maximum number of periods per day (for bitset)
    
    // ============================================================================
    // FORWARD DECLARATIONS
    // ============================================================================
    
    static inline int get_required_periods(const ProblemData &data, const string &course_id, const string &section_id);
    template<typename F>
    static inline void for_each_slot(const OptimalSolution::Assignment &a, const ProblemData &data, F fn);
    
    // ============================================================================
    // TYPE DEFINITIONS - Penalty and State Management
    // ============================================================================
    
    // PenaltyState: Tracks penalty-related state (workload, compactness)
    struct PenaltyState {
        unordered_map<string, int> workload;  // Teacher ID -> total workload (periods)
        unordered_map<string, unordered_map<string, bitset<MAX_PERIOD_INDEX>>> day_slots; // Teacher -> Day -> busy periods
        double workload_var = 0.0;            // Variance of workload distribution
        double sum_workload = 0.0;            // Sum of all workloads (∑X)
        double sum_workload_squared = 0.0;    // Sum of squared workloads (∑X²)
        double compactness = 0.0;             // Total compactness penalty across all teachers
        
        double get_workload_penalty() const {
            int num_teachers = (int)workload.size();
            return (num_teachers > 0) ? sqrt(workload_var / num_teachers) : 0.0;
        }
        
        // Compute variance from sum and sum_squared: var = (∑X² - (∑X)²/N) / N
        void update_workload_var_from_sums() {
            int num_teachers = (int)workload.size();
            if (num_teachers > 0) {
                double mean = sum_workload / num_teachers;
                workload_var = (sum_workload_squared / num_teachers) - (mean * mean);
                if (workload_var < 0.0) workload_var = 0.0;  // Numerical stability
            } else {
                workload_var = 0.0;
            }
        }
        
        static double compute_compactness_for_bitset(const bitset<MAX_PERIOD_INDEX> &slots) {
            if (slots.none()) return 0.0;
            vector<int> periods;
            periods.reserve(MAX_PERIOD_INDEX);
            for (int i = 0; i < MAX_PERIOD_INDEX; ++i) {
                if (slots.test(i)) periods.push_back(i);
            }
            if (periods.empty()) return 0.0;
            double penalty = 0.0;
            for (size_t i = 0; i < periods.size() - 1; ++i) {
                int gap = periods[i + 1] - periods[i];
                if (gap > 1) penalty += gap - 1;
            }
            return penalty;
        }
        
        double compute_compactness_for_teacher_day(const string &teacher_id, const string &day) const {
            auto it_teacher = day_slots.find(teacher_id);
            if (it_teacher == day_slots.end()) return 0.0;
            auto it_day = it_teacher->second.find(day);
            if (it_day == it_teacher->second.end()) return 0.0;
            return compute_compactness_for_bitset(it_day->second);
        }
        
        void update_workload(const AssignmentChange &chg, const ProblemData &data) {
            int old_p = get_required_periods(data, chg.old_a.course_id, chg.old_a.section_id);
            int new_p = get_required_periods(data, chg.new_a.course_id, chg.new_a.section_id);
            
            // Get old workload values before update
            int w_old_old = workload.count(chg.old_a.teacher_id) ? workload[chg.old_a.teacher_id] : 0;
            int w_old_new = workload.count(chg.new_a.teacher_id) ? workload[chg.new_a.teacher_id] : 0;
            bool same_teacher = (chg.old_a.teacher_id == chg.new_a.teacher_id);
            
            // Update sums: O(1) operations
            // Remove old contributions from sums
            if (w_old_old > 0) {
                sum_workload -= w_old_old;
                sum_workload_squared -= (double)w_old_old * w_old_old;
            }
            if (!same_teacher && w_old_new > 0) {
                sum_workload -= w_old_new;
                sum_workload_squared -= (double)w_old_new * w_old_new;
            }
            
            // Update workload map
            workload[chg.old_a.teacher_id] -= old_p;
            bool old_teacher_removed = (workload[chg.old_a.teacher_id] <= 0);
            if (old_teacher_removed) workload.erase(chg.old_a.teacher_id);
            workload[chg.new_a.teacher_id] += new_p;
            
            // Get new workload values after update
            int w_new_old = old_teacher_removed ? 0 : workload[chg.old_a.teacher_id];
            int w_new_new = workload[chg.new_a.teacher_id];
            
            // Add new contributions to sums
            if (same_teacher) {
                // Same teacher: workload changes from w_old_old to w_old_old - old_p + new_p
                if (w_new_new > 0) {
                    sum_workload += w_new_new;
                    sum_workload_squared += (double)w_new_new * w_new_new;
                }
            } else {
                // Different teachers
                if (w_new_old > 0) {
                    sum_workload += w_new_old;
                    sum_workload_squared += (double)w_new_old * w_new_old;
                }
                if (w_new_new > 0) {
                    sum_workload += w_new_new;
                    sum_workload_squared += (double)w_new_new * w_new_new;
                }
            }
            
            // Recompute variance from sums: O(1)
            update_workload_var_from_sums();
        }
        
        void update_compactness(const AssignmentChange &chg, const ProblemData &data) {
            double old_compact_old = compute_compactness_for_teacher_day(chg.old_a.teacher_id, chg.old_a.day);
            for_each_slot(chg.old_a, data, [&](const Slot &slot) {
                if (slot.period_idx >= 0 && slot.period_idx < MAX_PERIOD_INDEX) {
                    day_slots[chg.old_a.teacher_id][chg.old_a.day].reset(slot.period_idx);
                }
            });
            double new_compact_old = compute_compactness_for_teacher_day(chg.old_a.teacher_id, chg.old_a.day);
            compactness += new_compact_old - old_compact_old;
            
            if (day_slots[chg.old_a.teacher_id][chg.old_a.day].none()) {
                day_slots[chg.old_a.teacher_id].erase(chg.old_a.day);
            }
            if (day_slots[chg.old_a.teacher_id].empty()) {
                day_slots.erase(chg.old_a.teacher_id);
            }
            
            double old_compact_new = compute_compactness_for_teacher_day(chg.new_a.teacher_id, chg.new_a.day);
            for_each_slot(chg.new_a, data, [&](const Slot &slot) {
                if (slot.period_idx >= 0 && slot.period_idx < MAX_PERIOD_INDEX) {
                    day_slots[chg.new_a.teacher_id][chg.new_a.day].set(slot.period_idx);
                }
            });
            double new_compact_new = compute_compactness_for_teacher_day(chg.new_a.teacher_id, chg.new_a.day);
            compactness += new_compact_new - old_compact_new;
        }
        
        void apply_change(const AssignmentChange &chg, const ProblemData &data) {
            update_workload(chg, data);
            update_compactness(chg, data);
        }
        
        void revert_change(const AssignmentChange &chg, const ProblemData &data) {
            AssignmentChange reversed_chg = chg;
            swap(reversed_chg.old_a, reversed_chg.new_a);
            apply_change(reversed_chg, data);
        }
    };

    // ============================================================================
    // CONSTANTS
    // ============================================================================
    
    constexpr int DEFAULT_TABU_TENURE = 50;
    constexpr int MIN_CHAIN_SIZE = 3;
    constexpr int TOP_K_TEACHERS = 3;
    constexpr double COMPACTNESS_WEIGHT = 2.0;
    constexpr double MAX_EXPONENT = 700.0;
    constexpr double MIN_EXPONENT = -700.0;
    constexpr double MIN_TEMPERATURE = 1e-4;
    constexpr double STABILITY_CORE_MULTIPLIER = 1.5;
    constexpr double STABILITY_TEACHER_PENALTY = 1.5;
    constexpr double STABILITY_TIMESLOT_PENALTY = 1.0;
    
    // ============================================================================
    // TYPE DEFINITIONS - Tabu Search
    // ============================================================================
    
    // SimpleTabu: Tabu list for preventing revisiting recent moves
    struct SimpleTabu {
        deque<string> tabu_queue_teacher;              // Queue of teacher change signatures
        deque<string> tabu_queue_timeslot;             // Queue of timeslot change signatures
        deque<string> tabu_queue_chain_teacher_day;    // Queue of chain move signatures
        unordered_set<string> tabu_set_teacher;        // Set of tabu teacher signatures
        unordered_set<string> tabu_set_timeslot;       // Set of tabu timeslot signatures
        unordered_set<string> tabu_set_chain_teacher_day; // Set of tabu chain signatures
        size_t tenure;  // Tabu tenure (how long moves stay tabu)

        SimpleTabu(size_t tenure_val = DEFAULT_TABU_TENURE) : tenure(tenure_val) {}

        bool is_tabu(const string &sig_teacher, const string &sig_timeslot, const string &sig_chain_teacher_day = "") const {
            return (!sig_teacher.empty() && tabu_set_teacher.count(sig_teacher)) ||
                   (!sig_timeslot.empty() && tabu_set_timeslot.count(sig_timeslot)) ||
                   (!sig_chain_teacher_day.empty() && tabu_set_chain_teacher_day.count(sig_chain_teacher_day));
        }

        void add_tabu(const string &sig_teacher, const string &sig_timeslot, const string &sig_chain_teacher_day = "") {
            auto add = [&](deque<string> &q, unordered_set<string> &s, const string &val) {
                if (!val.empty()) {
                    q.push_back(val);
                    s.insert(val);
                    if (q.size() > tenure) {
                        s.erase(q.front());
                        q.pop_front();
                    }
                }
            };
            add(tabu_queue_teacher, tabu_set_teacher, sig_teacher);
            add(tabu_queue_timeslot, tabu_set_timeslot, sig_timeslot);
            add(tabu_queue_chain_teacher_day, tabu_set_chain_teacher_day, sig_chain_teacher_day);
        }
    };
    
    // ============================================================================
    // TYPE DEFINITIONS - Result Structures
    // ============================================================================
    
    // TryAssignmentResult: Result of attempting to assign an assignment
    struct TryAssignmentResult {
        bool ok;                    // Whether the assignment is feasible
        string classroom_id;        // Assigned classroom ID (empty if not feasible)
        IndexDelta delta;           // Index delta for this change
    };
    
    // AcceptMoveResult: Result of move acceptance decision
    struct AcceptMoveResult {
        bool accepted;              // Whether the move was accepted
        bool rejected_by_tabu;      // Whether rejected due to tabu
        bool rejected_by_sa;        // Whether rejected by simulated annealing
        string sig_teacher;          // Teacher change signature
        string sig_timeslot;        // Timeslot change signature
        string sig_chain_teacher_day; // Chain move signature
    };
    
    mt19937 rng(chrono::steady_clock::now().time_since_epoch().count());

    // ============================================================================
    // UTILITY FUNCTIONS
    // ============================================================================
    
    // ----------------------------------------------------------------------------
    // Index Conversion Functions
    // ----------------------------------------------------------------------------
    
    // Convert day_idx and period_idx to a single integer slot index for fast lookup
    // Formula: slot_index = day_idx * num_periods + period_idx
    static inline int slot_index(int day_idx, int period_idx, int num_periods) {
        return day_idx * num_periods + period_idx;
    }
    
    // Find the index of a period string in the periods vector
    static inline int find_period_index(const vector<string> &periods, const string &period) {
        for (int m = 0; m < (int)periods.size(); ++m)
            if (periods[m] == period) return m;
        return -1;
    }
    
    // Find the index of a day string in the days vector
    static inline int find_day_index(const vector<string> &days, const string &day) {
        for (int d = 0; d < (int)days.size(); ++d)
            if (days[d] == day) return d;
        return -1;
    }
    
    // ----------------------------------------------------------------------------
    // Data Lookup Functions
    // ----------------------------------------------------------------------------
    
    // Get the number of required periods for a course section
    static inline int get_required_periods(const ProblemData &data, const string &course_id, const string &section_id) {
        for (const auto &c : data.courses)
            if (c.id == course_id)
                for (const auto &s : c.sections)
                    if (s.id == section_id) return s.required_periods;
        return 1;
    }
    
    // Get the number of required seats for a course section
    static inline int get_required_seats(const ProblemData &data, const string &course_id, const string &section_id) {
        for (const auto &c : data.courses)
            if (c.id == course_id)
                for (const auto &s : c.sections)
                    if (s.id == section_id) return s.required_seats;
        return 0;
    }
    
    // Find a course by its ID, returns nullptr if not found
    static const Course* find_course(const ProblemData &data, const string &course_id) {
        for (const auto &c : data.courses)
            if (c.id == course_id) return &c;
        return nullptr;
    }
    
    // Generate a unique assignment ID string from course_id and section_id
    static inline string get_assignment_id(const OptimalSolution::Assignment &a) {
        return a.course_id + "|" + a.section_id;
    }
    
    // ----------------------------------------------------------------------------
    // Random Selection Functions
    // ----------------------------------------------------------------------------
    
    // Select a random assignment index from the solution
    static int select_random_assignment(const OptimalSolution &sol) {
        if (sol.assignments.empty()) return -1;
        uniform_int_distribution<int> dist(0, (int)sol.assignments.size() - 1);
        return dist(rng);
    }
    
    // Select a random timeslot (day, period) from available timeslots
    static pair<string, string> select_random_timeslot(const ProblemData &data) {
        uniform_int_distribution<int> ldist(0, (int)data.classrooms.days.size() - 1);
        uniform_int_distribution<int> pdist(0, (int)data.classrooms.periods.size() - 1);
        return {data.classrooms.days[ldist(rng)], data.classrooms.periods[pdist(rng)]};
    }
    
    // Select a random teacher from the given list
    static string select_random_teacher(const vector<string> &teachers) {
        if (teachers.empty()) return "";
        uniform_int_distribution<int> tdist(0, (int)teachers.size() - 1);
        return teachers[tdist(rng)];
    }
    
    // ----------------------------------------------------------------------------
    // Slot Iteration Functions
    // ----------------------------------------------------------------------------
    
    // Iterate over all time slots occupied by an assignment
    // Calls the function fn for each slot (day_idx, period_idx) that the assignment occupies
    template<class F> 
    static inline void for_each_slot(const OptimalSolution::Assignment &a, const ProblemData &data, F fn) {
        int day_idx = find_day_index(data.classrooms.days, a.day);
        int period_idx = find_period_index(data.classrooms.periods, a.period);
        int required_periods = get_required_periods(data, a.course_id, a.section_id);
        
        if (day_idx < 0 || period_idx < 0) return;
        if (period_idx + required_periods > (int)data.classrooms.periods.size()) return;
        
        for (int i = 0; i < required_periods; ++i) {
            fn(Slot{day_idx, period_idx + i});
        }
    }
    
    // ----------------------------------------------------------------------------
    // Statistical Calculation Functions
    // ----------------------------------------------------------------------------
    
    // Compute standard deviation of a vector of integers
    static inline double compute_stddev(const vector<int> &vals) {
        if (vals.empty()) return 0.0;
        double mean = accumulate(vals.begin(), vals.end(), 0.0) / vals.size();
        double s = 0.0;
        for (double v : vals) s += (v - mean) * (v - mean);
        return sqrt(s / vals.size());
    }
    
    // ============================================================================
    // SIMULATED ANNEALING - Move Acceptance and Solution Update
    // ============================================================================
    
    // ----------------------------------------------------------------------------
    // Move Acceptance Decision
    // ----------------------------------------------------------------------------
    
    // Determine if a move should be accepted based on simulated annealing criteria
    // SA acceptance rule: accept if delta >= 0 (improving), or with probability exp(delta/T) if delta < 0
    // Returns true if move should be accepted, false otherwise
    // 
    // Parameters:
    //   objective_delta: Change in objective value (new - old), positive = improvement
    //   temperature: Current SA temperature (controls acceptance probability)
    //   sa_rejected: Counter for rejected moves (incremented on rejection)
    //   best_score: Best objective value found so far
    //   candidate_score: Objective value of candidate solution
    //   is_at_best: Whether current solution is at best (for escape mechanism)
    static bool should_accept_move(double objective_delta, double temperature, int &sa_rejected, 
                                   double best_score = 0.0, double candidate_score = 0.0, bool is_at_best = false) {
        // Always accept improving moves (delta >= 0)
        if (objective_delta >= 0.0) return true;
        
        // Escape mechanism: When temperature is very low and candidate improves best,
        // be more lenient to accept small negative deltas (helps escape local optima)
        if (temperature < MIN_TEMPERATURE * 10 && candidate_score > best_score) {
            if (objective_delta > -0.1) return true;  // Accept small negative deltas that improve best
        }
        
        // Escape mechanism: When stuck at best with very low temperature,
        // accept slightly worse moves to escape (diversification)
        if (is_at_best && temperature < MIN_TEMPERATURE * 100) {
            if (objective_delta > -0.05) return true;  // Accept very small negative deltas to escape
        }
        
        // Standard SA acceptance: P(accept) = exp(delta / T)
        // For negative delta, this gives probability < 1
        double acceptance_exponent = objective_delta / temperature;
        
        // Clamp exponent to avoid numerical overflow/underflow
        if (acceptance_exponent > MAX_EXPONENT) return true;  // Very high probability, accept
        if (acceptance_exponent < MIN_EXPONENT) {
            sa_rejected++;  // Very low probability, reject
            return false;
        }
        
        // Random acceptance based on probability exp(delta/T)
        uniform_real_distribution<double> random_uniform(0.0, 1.0);
        double acceptance_probability = exp(acceptance_exponent);
        if (random_uniform(rng) < acceptance_probability) {
            return true;  // Accepted by probability
        }
        
        sa_rejected++;  // Rejected by probability
        return false;
    }
    
    // Extract tabu keys (signatures) from a move for tabu list management
    static tuple<string, string, string> extract_tabu_keys(const OptimalSolution &old_sol, const OptimalSolution &new_sol, const Move &move) {
        string sig_teacher = "", sig_timeslot = "", sig_chain_teacher_day = "";
        auto check_idx = [&](int idx) -> const OptimalSolution::Assignment* {
            if (idx < 0 || idx >= (int)old_sol.assignments.size() || idx >= (int)new_sol.assignments.size()) return nullptr;
            const auto &old_a = old_sol.assignments[idx], &new_a = new_sol.assignments[idx];
            if (old_a.course_id != new_a.course_id || old_a.section_id != new_a.section_id) return nullptr;
            return &old_a;
        };
        if (move.indices.empty()) return make_tuple("", "", "");
        if (move.type == Move::SINGLE_CHANGE || move.type == Move::BLOCK_RELOCATE) {
            const auto *old_a = check_idx(move.indices[0]);
            if (!old_a) return make_tuple("", "", "");
            const auto &new_a = new_sol.assignments[move.indices[0]];
            string aid = get_assignment_id(*old_a);
            if (old_a->teacher_id != new_a.teacher_id)
                sig_teacher = aid + "|" + old_a->teacher_id + "->" + new_a.teacher_id;
            if (old_a->day != new_a.day || old_a->period != new_a.period) {
                string old_slot = old_a->day + "|" + old_a->period, new_slot = new_a.day + "|" + new_a.period;
                sig_timeslot = aid + "|" + min(old_slot, new_slot) + "|" + max(old_slot, new_slot);
            }
        } else if (move.type == Move::CHAIN_MOVE) {
            if (!move.chain.empty()) {
                const auto &first_block = move.chain[0];
                if (first_block.assignment_idx >= 0 && first_block.assignment_idx < (int)old_sol.assignments.size()) {
                    const auto &a = old_sol.assignments[first_block.assignment_idx];
                    sig_chain_teacher_day = a.teacher_id + "|" + a.day + "|CHAIN";
                }
            }
            for (const auto &block : move.chain) {
                const auto *old_a = check_idx(block.assignment_idx);
                if (!old_a) continue;
                const auto &new_a = new_sol.assignments[block.assignment_idx];
                if (old_a->day != new_a.day || old_a->period != new_a.period) {
                    if (!sig_timeslot.empty()) sig_timeslot += ";";
                    string aid = get_assignment_id(*old_a);
                    string old_slot = old_a->day + "|" + old_a->period, new_slot = new_a.day + "|" + new_a.period;
                    sig_timeslot += aid + "|" + min(old_slot, new_slot) + "|" + max(old_slot, new_slot);
                }
            }
        }
        return make_tuple(sig_teacher, sig_timeslot, sig_chain_teacher_day);
    }
    
    // ============================================================================
    // CHAIN MOVES AND BLOCK MOVES
    // ============================================================================
    //
    // Chain Moves: Rotate a sequence of consecutive assignments for the same teacher on the same day.
    // Purpose: Improve compactness by rotating periods while maintaining teacher-day assignments.
    //          Helps reduce gaps between periods (compactness penalty) without changing teacher assignments.
    //
    // Block Moves: Relocate a single assignment to a new timeslot.
    // Purpose: Diversification and exploration of solution space.
    //
    // Penalty Updates: Both moves update workload (teacher periods) and compactness (period gaps).
    //                  Chain moves preserve teacher assignments but change period distribution.
    //                  Block moves may change both teacher and timeslot, affecting all penalties.
    // ============================================================================
    
    // ----------------------------------------------------------------------------
    // Chain Extraction and Validation
    // ----------------------------------------------------------------------------
    
    // Extract a chain of consecutive assignments for a teacher on a specific day
    // A chain is valid if:
    //   1. All assignments have the same teacher and day
    //   2. All assignments have the same required_periods (same block size)
    //   3. Assignments are consecutive (no gaps between periods)
    //   4. Chain size >= MIN_CHAIN_SIZE
    //
    // Returns: Vector of Block structures representing the chain, or empty if invalid
    static vector<Block> extract_chain(const string &teacher_id, const string &day, 
                                       const OptimalSolution &sol, const ProblemData &data) {
        vector<Block> chain;
        
        // Collect all assignments for this teacher-day combination
        for (int assignment_idx = 0; assignment_idx < (int)sol.assignments.size(); ++assignment_idx) {
            const auto &assignment = sol.assignments[assignment_idx];
            if (assignment.teacher_id == teacher_id && assignment.day == day) {
                int required_periods = get_required_periods(data, assignment.course_id, assignment.section_id);
                chain.push_back({assignment_idx, assignment.period, required_periods});
            }
        }
        
        // Sort by period index to check consecutiveness
        sort(chain.begin(), chain.end(), [&](const Block &block_a, const Block &block_b) {
            int period_idx_a = find_period_index(data.classrooms.periods, block_a.period);
            int period_idx_b = find_period_index(data.classrooms.periods, block_b.period);
            return period_idx_a < period_idx_b;
        });
        
        // Validate chain size
        if (chain.size() < MIN_CHAIN_SIZE) return {};
        
        // Validate all blocks have same required_periods (same block size)
        int common_required_periods = chain[0].required_periods;
        for (const auto &block : chain) {
            if (block.required_periods != common_required_periods) return {};
        }
        
        // Validate consecutiveness: each block must start immediately after previous block ends
        for (size_t i = 1; i < chain.size(); ++i) {
            int prev_period_idx = find_period_index(data.classrooms.periods, chain[i-1].period);
            int curr_period_idx = find_period_index(data.classrooms.periods, chain[i].period);
            
            if (prev_period_idx < 0 || curr_period_idx < 0) return {};  // Invalid period indices
            
            // Current block must start exactly where previous block ends
            int expected_curr_period_idx = prev_period_idx + common_required_periods;
            if (curr_period_idx != expected_curr_period_idx) return {};  // Not consecutive
        }
        
        return chain;
    }
    
    // ----------------------------------------------------------------------------
    // Chain Rotation
    // ----------------------------------------------------------------------------
    
    // Rotate a chain of assignments: each assignment takes the period of the previous one
    // This creates a circular shift that maintains the chain structure while changing period distribution
    // 
    // Example: [P1, P2, P3] -> [P3, P1, P2] (each moves to previous position)
    //
    // Purpose: Change compactness without changing teacher assignments
    //          May improve or worsen compactness, allowing exploration
    static void rotate_chain(vector<Block> &chain, const ProblemData &data) {
        if (chain.empty()) return;
        
        int common_required_periods = chain[0].required_periods;
        
        // Save original periods before rotation
        vector<string> original_periods(chain.size());
        for (size_t i = 0; i < chain.size(); ++i) {
            original_periods[i] = chain[i].period;
        }
        
        // Rotate: each block gets the period of the previous block (circular)
        for (size_t i = 0; i < chain.size(); ++i) {
            size_t prev_block_idx = (i - 1 + chain.size()) % chain.size();  // Circular: last -> first
            int new_start_period_idx = find_period_index(data.classrooms.periods, original_periods[prev_block_idx]);
            
            // Validate new period is within bounds
            if (new_start_period_idx < 0 || new_start_period_idx + common_required_periods > (int)data.classrooms.periods.size()) {
                chain.clear();  // Invalid rotation, clear chain
                return;
            }
            
            chain[i].period = data.classrooms.periods[new_start_period_idx];
        }
    }
    
    // ============================================================================
    // FORWARD DECLARATIONS FOR MOVE-RELATED FUNCTIONS
    // ============================================================================
    
    static vector<AssignmentChange> expand_move(const Move &move, const OptimalSolution &current, const OptimalSolution &candidate);

    class MoveBuilder {
    private:
        MoveSpec spec;
        const OptimalSolution &sol;
        const ProblemData &data;
        
    public:
        MoveBuilder(const OptimalSolution &sol_ref, const ProblemData &data_ref) : sol(sol_ref), data(data_ref) { spec = MoveSpec(); }
        
        bool build_single_change() {
            int idx_assign = select_random_assignment(sol);
            if (idx_assign < 0) return false;
            const auto &a = sol.assignments[idx_assign];
            spec.assignment_indices = {idx_assign};
            spec.type = Move::SINGLE_CHANGE;
            const Course *course = find_course(data, a.course_id);
            if (!course || course->Ij.empty()) return false;
            string new_teacher = select_random_teacher(course->Ij);
            if (new_teacher != a.teacher_id) {
                spec.new_teacher_id = new_teacher;
                return true;
            }
            auto [new_day, new_period] = select_random_timeslot(data);
            spec.new_day = new_day;
            spec.new_period = new_period;
            return true;
        }
        
        // ------------------------------------------------------------------------
        // Block Move: Relocate Assignment
        // ------------------------------------------------------------------------
        
        // Build a block relocate move: move a single assignment to a new random timeslot
        // This changes the assignment's day and period, potentially changing teacher if needed
        // Penalty impact: May change workload (if teacher changes) and compactness (period gaps)
        bool build_block_relocate() {
            int assignment_idx = select_random_assignment(sol);
            if (assignment_idx < 0) return false;
            
            spec.assignment_indices = {assignment_idx};
            spec.type = Move::BLOCK_RELOCATE;
            
            // Select random new timeslot
            auto [new_day, new_period] = select_random_timeslot(data);
            spec.new_day = new_day;
            spec.new_period = new_period;
            
            return true;
        }
        
        // ------------------------------------------------------------------------
        // Chain Move: Rotate Consecutive Assignments
        // ------------------------------------------------------------------------
        
        // Build a chain move: rotate consecutive assignments for a teacher on a specific day
        // Selection strategy: Prefer teachers with high workload imbalance or compactness issues
        // 
        // Penalty impact:
        //   - Workload: Unchanged (same teacher, same total periods)
        //   - Compactness: May improve or worsen depending on new period distribution
        //   - Teacher/classroom: Unchanged (only periods rotate)
        bool build_chain_move(const PenaltyState &penalty_state) {
            if (data.teachers.empty() || data.classrooms.days.empty()) return false;
            
            spec.type = Move::CHAIN_MOVE;
            vector<pair<string, double>> teacher_penalty_scores;
            
            // Calculate mean workload for scoring
            double mean_workload = 0.0;
            if (!penalty_state.workload.empty()) {
                double total_workload = 0.0;
                for (const auto &[teacher_id, workload] : penalty_state.workload) {
                    total_workload += workload;
                }
                mean_workload = total_workload / penalty_state.workload.size();
            }
            
            // Score each teacher based on workload imbalance and compactness issues
            // Higher score = more likely to benefit from chain move
            for (const auto &teacher : data.teachers) {
                double penalty_score = 0.0;
                
                // Workload imbalance: deviation from mean
                auto workload_it = penalty_state.workload.find(teacher.id);
                if (workload_it != penalty_state.workload.end()) {
                    double workload_deviation = abs(workload_it->second - mean_workload);
                    penalty_score += workload_deviation;
                }
                
                // Compactness issues: gaps between periods
                auto day_slots_it = penalty_state.day_slots.find(teacher.id);
                if (day_slots_it != penalty_state.day_slots.end()) {
                    for (const auto &[day, period_slots] : day_slots_it->second) {
                        double compactness_penalty = PenaltyState::compute_compactness_for_bitset(period_slots);
                        penalty_score += compactness_penalty * COMPACTNESS_WEIGHT;
                    }
                }
                
                if (penalty_score > 0.0) {
                    teacher_penalty_scores.push_back({teacher.id, penalty_score});
                }
            }
            
            // Select teacher: prefer high-penalty teachers, but allow random selection
            string selected_teacher_id;
            if (teacher_penalty_scores.empty()) {
                // No teachers with penalties, select randomly
                vector<string> all_teacher_ids;
                all_teacher_ids.reserve(data.teachers.size());
                for (const auto &teacher : data.teachers) {
                    all_teacher_ids.push_back(teacher.id);
                }
                selected_teacher_id = select_random_teacher(all_teacher_ids);
            } else {
                // Sort by penalty score (highest first) and select from top K
                sort(teacher_penalty_scores.begin(), teacher_penalty_scores.end(), 
                     [](const pair<string, double> &a, const pair<string, double> &b) { 
                         return a.second > b.second;  // Higher penalty = higher priority
                     });
                int top_k_count = min(TOP_K_TEACHERS, (int)teacher_penalty_scores.size());
                uniform_int_distribution<int> teacher_dist(0, top_k_count - 1);
                selected_teacher_id = teacher_penalty_scores[teacher_dist(rng)].first;
            }
            
            // Select random day for chain extraction
            uniform_int_distribution<int> day_dist(0, (int)data.classrooms.days.size() - 1);
            string selected_day = data.classrooms.days[day_dist(rng)];
            
            // Extract and validate chain
            spec.chain = extract_chain(selected_teacher_id, selected_day, sol, data);
            if (spec.chain.size() < MIN_CHAIN_SIZE) return false;
            
            // Rotate chain to create the move
            rotate_chain(spec.chain, data);
            if (spec.chain.empty()) return false;  // Rotation may invalidate chain
            
            // Collect assignment indices from chain blocks
            spec.assignment_indices.clear();
            for (const auto &block : spec.chain) {
                spec.assignment_indices.push_back(block.assignment_idx);
            }
            
            return true;
        }
        
        // ------------------------------------------------------------------------
        // Multi-Swap: Diversification Move
        // ------------------------------------------------------------------------
        
        // Build a block relocate move for diversification when search is stuck
        // This is similar to build_block_relocate but with shuffled selection
        // Purpose: Escape local optima by trying random relocations
        //
        // Penalty impact: Same as block relocate (may change workload and compactness)
        bool build_multi_swap(int num_swaps = 2) {
            if (sol.assignments.size() < num_swaps) return false;
            
            spec.type = Move::BLOCK_RELOCATE;
            spec.assignment_indices.clear();
            
            // Shuffle assignment indices for random selection
            vector<int> available_indices;
            for (int i = 0; i < (int)sol.assignments.size(); ++i) {
                available_indices.push_back(i);
            }
            shuffle(available_indices.begin(), available_indices.end(), rng);
            
            // Select first assignment from shuffled list for relocation
            if (available_indices.empty()) return false;
            int selected_assignment_idx = available_indices[0];
            spec.assignment_indices = {selected_assignment_idx};
            
            // Select random new timeslot
            auto [new_day, new_period] = select_random_timeslot(data);
            spec.new_day = new_day;
            spec.new_period = new_period;
            
            return true;
        }
        
        MoveSpec get_spec() const { return spec; }
        bool build(Move::Type move_type, const PenaltyState &state) {
            switch (move_type) {
                case Move::SINGLE_CHANGE: return build_single_change();
                case Move::BLOCK_RELOCATE: return build_block_relocate();
                case Move::CHAIN_MOVE: return build_chain_move(state);
                default: return false;
            }
        }
    };

    static SolIndex build_index(const OptimalSolution &sol, const ProblemData &data) {
        SolIndex idx;
        idx.num_periods = (int)data.classrooms.periods.size();
        for (const auto &a : sol.assignments) {
            for_each_slot(a, data, [&](const Slot &slot) {
                int slot_idx = slot_index(slot.day_idx, slot.period_idx, idx.num_periods);
                idx.teacher_busy_idx[a.teacher_id].insert(slot_idx);
                idx.course_slot_section_idx[a.course_id][slot_idx] = a.section_id;
                idx.course_teachers[a.course_id].insert(a.teacher_id);
                idx.course_teacher_sections[a.course_id + "|" + a.teacher_id].insert(a.section_id);
                if (!a.classroom_id.empty()) idx.classroom_busy_idx[a.classroom_id].insert(slot_idx);
            });
        }
        return idx;
    }

    static void apply_index_delta(SolIndex &idx, const IndexDelta &d) {
        auto erase_if_empty = [](auto &map, const auto &key) {
            if (map[key].empty()) map.erase(key);
        };
        for (const auto &[teacher_id, slot_idx] : d.teacher_remove_idx) {
            idx.teacher_busy_idx[teacher_id].erase(slot_idx);
            erase_if_empty(idx.teacher_busy_idx, teacher_id);
        }
        for (const auto &[teacher_id, slot_idx] : d.teacher_add_idx)
            idx.teacher_busy_idx[teacher_id].insert(slot_idx);
        for (const auto &[course_id, slot_idx, section_id] : d.course_slot_section_remove_idx) {
            idx.course_slot_section_idx[course_id].erase(slot_idx);
            erase_if_empty(idx.course_slot_section_idx, course_id);
        }
        for (const auto &[course_id, slot_idx, section_id] : d.course_slot_section_add_idx)
            idx.course_slot_section_idx[course_id][slot_idx] = section_id;
        for (const auto &[course_id, teacher_id, section_id] : d.course_teacher_section_remove) {
            string ctk = course_id + "|" + teacher_id;
            idx.course_teacher_sections[ctk].erase(section_id);
            if (idx.course_teacher_sections[ctk].empty()) {
                idx.course_teacher_sections.erase(ctk);
                idx.course_teachers[course_id].erase(teacher_id);
                erase_if_empty(idx.course_teachers, course_id);
            }
        }
        for (const auto &[course_id, teacher_id, section_id] : d.course_teacher_section_add) {
            string ctk = course_id + "|" + teacher_id;
            if (idx.course_teacher_sections[ctk].empty())
                idx.course_teachers[course_id].insert(teacher_id);
            idx.course_teacher_sections[ctk].insert(section_id);
        }
        for (const auto &[classroom_id, slot_idx] : d.classroom_remove_idx) {
            idx.classroom_busy_idx[classroom_id].erase(slot_idx);
            erase_if_empty(idx.classroom_busy_idx, classroom_id);
        }
        for (const auto &[classroom_id, slot_idx] : d.classroom_add_idx)
            idx.classroom_busy_idx[classroom_id].insert(slot_idx);
    }

    static IndexDelta build_delta(
        const OptimalSolution::Assignment &old_a,
        const OptimalSolution::Assignment &new_a,
        DeltaMode mode,
        const ProblemData &data
    ) {
        IndexDelta delta;
        int num_periods = (int)data.classrooms.periods.size();
        
        auto process_slot = [&](const OptimalSolution::Assignment &a, bool is_add) {
            for_each_slot(a, data, [&](const Slot &slot) {
                int slot_idx = slot_index(slot.day_idx, slot.period_idx, num_periods);
                if (is_add) {
                    delta.teacher_add_idx.push_back({a.teacher_id, slot_idx});
                    delta.course_slot_section_add_idx.push_back({a.course_id, slot_idx, a.section_id});
                    delta.course_teacher_section_add.push_back({a.course_id, a.teacher_id, a.section_id});
                    if (!a.classroom_id.empty()) {
                        delta.classroom_add_idx.push_back({a.classroom_id, slot_idx});
                    }
                } else {
                    delta.teacher_remove_idx.push_back({a.teacher_id, slot_idx});
                    delta.course_slot_section_remove_idx.push_back({a.course_id, slot_idx, a.section_id});
                    delta.course_teacher_section_remove.push_back({a.course_id, a.teacher_id, a.section_id});
                    if (!a.classroom_id.empty()) {
                        delta.classroom_remove_idx.push_back({a.classroom_id, slot_idx});
                    }
                }
            });
        };
        
        switch (mode) {
            case DeltaMode::ADD:
                process_slot(new_a, true);
                break;
            case DeltaMode::REMOVE:
                process_slot(old_a, false);
                break;
            case DeltaMode::DIFF:
                process_slot(old_a, false);
                process_slot(new_a, true);
                break;
        }
        
        return delta;
    }

    static string is_feasible(const OptimalSolution::Assignment &a,
                              const SolIndex &idx, const ProblemData &data) {
        int day_idx = find_day_index(data.classrooms.days, a.day);
        int period_idx = find_period_index(data.classrooms.periods, a.period);
        int required_periods = get_required_periods(data, a.course_id, a.section_id);
        
        if (day_idx < 0 || period_idx < 0 || period_idx + required_periods > (int)data.classrooms.periods.size()) {
            return "";
        }
        
        auto it_teacher = idx.teacher_busy_idx.find(a.teacher_id);
        if (it_teacher != idx.teacher_busy_idx.end()) {
            bool has_clash = false;
            for_each_slot(a, data, [&](const Slot &slot) {
                int slot_idx = slot_index(slot.day_idx, slot.period_idx, idx.num_periods);
                if (it_teacher->second.count(slot_idx)) has_clash = true;
            });
            if (has_clash) return "";
        }
        
        auto it_course = idx.course_slot_section_idx.find(a.course_id);
        if (it_course != idx.course_slot_section_idx.end()) {
            bool has_clash = false;
            for_each_slot(a, data, [&](const Slot &slot) {
                int slot_idx = slot_index(slot.day_idx, slot.period_idx, idx.num_periods);
                auto it_slot = it_course->second.find(slot_idx);
                if (it_slot != it_course->second.end() && it_slot->second != a.section_id) {
                    has_clash = true;
                }
            });
            if (has_clash) return "";
        }
        
        int required_seats = get_required_seats(data, a.course_id, a.section_id);
        if (data.classrooms.classrooms.empty()) return "";
        
        for (const auto &room : data.classrooms.classrooms) {
            if (room.capacity < required_seats) continue;
            auto it_room = idx.classroom_busy_idx.find(room.id);
            bool available = true;
            for_each_slot(a, data, [&](const Slot &slot) {
                int slot_idx = slot_index(slot.day_idx, slot.period_idx, idx.num_periods);
                if (it_room != idx.classroom_busy_idx.end() && it_room->second.count(slot_idx)) {
                    available = false;
                }
            });
            if (available) return room.id;
        }
        return "";
    }

    static string is_feasible_with_removal(
        const OptimalSolution::Assignment &old_a,
        const OptimalSolution::Assignment &new_a,
        const SolIndex &idx,
        const ProblemData &data
    ) {
        int day_idx = find_day_index(data.classrooms.days, new_a.day);
        int period_idx = find_period_index(data.classrooms.periods, new_a.period);
        int required_periods = get_required_periods(data, new_a.course_id, new_a.section_id);
        
        if (day_idx < 0 || period_idx < 0 || period_idx + required_periods > (int)data.classrooms.periods.size()) {
            return "";
        }
        
        // Use integer-based indices for fast lookup
        unordered_set<int> old_slots;
        for_each_slot(old_a, data, [&](const Slot &slot) {
            int slot_idx = slot_index(slot.day_idx, slot.period_idx, idx.num_periods);
            old_slots.insert(slot_idx);
        });
        
        bool same_teacher = (old_a.teacher_id == new_a.teacher_id);
        auto it_teacher = idx.teacher_busy_idx.find(new_a.teacher_id);
        if (it_teacher != idx.teacher_busy_idx.end()) {
            bool has_clash = false;
            for_each_slot(new_a, data, [&](const Slot &slot) {
                int slot_idx = slot_index(slot.day_idx, slot.period_idx, idx.num_periods);
                bool is_old_slot = old_slots.count(slot_idx) > 0;
                if (it_teacher->second.count(slot_idx) && (!same_teacher || !is_old_slot)) {
                    has_clash = true;
                }
            });
            if (has_clash) return "";
        }
        
        bool same_course = (old_a.course_id == new_a.course_id);
        auto it_course = idx.course_slot_section_idx.find(new_a.course_id);
        if (it_course != idx.course_slot_section_idx.end()) {
            bool has_clash = false;
            for_each_slot(new_a, data, [&](const Slot &slot) {
                int slot_idx = slot_index(slot.day_idx, slot.period_idx, idx.num_periods);
                bool is_old_slot = old_slots.count(slot_idx) > 0;
                auto it_slot = it_course->second.find(slot_idx);
                if (it_slot != it_course->second.end() && it_slot->second != new_a.section_id && (!same_course || !is_old_slot)) {
                    has_clash = true;
                }
            });
            if (has_clash) return "";
        }
        
        int required_seats = get_required_seats(data, new_a.course_id, new_a.section_id);
        if (data.classrooms.classrooms.empty()) return "";
        
        bool old_has_classroom = !old_a.classroom_id.empty();
        for (const auto &room : data.classrooms.classrooms) {
            if (room.capacity < required_seats) continue;
            auto it_room = idx.classroom_busy_idx.find(room.id);
            bool available = true;
            bool same_classroom = (old_has_classroom && old_a.classroom_id == room.id);
            for_each_slot(new_a, data, [&](const Slot &slot) {
                int slot_idx = slot_index(slot.day_idx, slot.period_idx, idx.num_periods);
                bool is_old_slot = old_slots.count(slot_idx) > 0;
                if (it_room != idx.classroom_busy_idx.end() && it_room->second.count(slot_idx) && (!same_classroom || !is_old_slot)) {
                    available = false;
                }
            });
            if (available) return room.id;
        }
        return "";
    }

    // ----------------------------------------------------------------------------
    // Assignment Change Evaluation with Classroom/Teacher Tracking
    // ----------------------------------------------------------------------------
    
    // Try to apply an assignment change and track all updates needed
    // 
    // This function:
    //   1. Checks feasibility of the change (considering removal of old assignment)
    //   2. Assigns a suitable classroom for the new assignment
    //   3. Builds index delta tracking all changes (teacher, classroom, course-slot mappings)
    //
    // Classroom/Teacher Updates Tracked in IndexDelta:
    //   - Teacher busy slots: Remove old slots, add new slots
    //   - Classroom busy slots: Remove old slots, add new slots (if classroom changes)
    //   - Course-slot-section mappings: Update for new timeslot
    //   - Course-teacher mappings: Update if teacher changes
    //
    // Returns: TryAssignmentResult with feasibility status, assigned classroom, and index delta
    static TryAssignmentResult try_assignment_change(
        const OptimalSolution::Assignment &old_assignment,
        OptimalSolution::Assignment new_assignment,
        const SolIndex &solution_index,
        const ProblemData &data
    ) {
        TryAssignmentResult result{false};
        
        // Check feasibility and find suitable classroom
        // is_feasible_with_removal considers that old_assignment will be removed,
        // so it allows new_assignment to use the same slots if they overlap
        result.classroom_id = is_feasible_with_removal(old_assignment, new_assignment, solution_index, data);
        
        if (result.classroom_id.empty()) {
            return result;  // Not feasible, return early
        }
        
        // Assignment is feasible: set classroom and build delta
        new_assignment.classroom_id = result.classroom_id;
        
        // Build index delta tracking all changes:
        //   - Teacher busy slots (add/remove)
        //   - Classroom busy slots (add/remove)
        //   - Course-slot-section mappings (add/remove)
        //   - Course-teacher mappings (add/remove if teacher changes)
        result.delta = build_delta(old_assignment, new_assignment, DeltaMode::DIFF, data);
        
        result.ok = true;
        return result;
    }

    static inline PenaltyState init_penalty_state(const OptimalSolution &sol, const ProblemData &data) {
        PenaltyState state;
        
        for (const auto &a : sol.assignments) {
            int required_periods = get_required_periods(data, a.course_id, a.section_id);
            state.workload[a.teacher_id] += required_periods;
            
            for_each_slot(a, data, [&](const Slot &slot) {
                if (slot.period_idx >= 0 && slot.period_idx < MAX_PERIOD_INDEX) {
                    state.day_slots[a.teacher_id][a.day].set(slot.period_idx);
                }
            });
        }
        
        for (const auto &[tid, days] : state.day_slots) {
            for (const auto &[day, slots] : days) {
                state.compactness += PenaltyState::compute_compactness_for_bitset(slots);
            }
        }
        
        // Initialize sums: O(N) only once during initialization
        state.sum_workload = 0.0;
        state.sum_workload_squared = 0.0;
        for (const auto &tw : state.workload) {
            int w = tw.second;
            state.sum_workload += w;
            state.sum_workload_squared += (double)w * w;
        }
        // Compute variance from sums: O(1)
        state.update_workload_var_from_sums();
        
        return state;
    }

    // ----------------------------------------------------------------------------
    // Helper Functions for Evaluation
    // ----------------------------------------------------------------------------
    
    // Get time preference score for an assignment
    // Uses precomputed time_pref_map: teacher_id -> slot_index -> score
    static double get_time_pref_score(const OptimalSolution::Assignment &a, int, int, 
                                     const ProblemData &data, 
                                     const unordered_map<string, unordered_map<int, int>> &time_pref_map) {
        double score = 0.0;
        int required_periods = get_required_periods(data, a.course_id, a.section_id);
        int start_period_idx = find_period_index(data.classrooms.periods, a.period);
        int day_idx = find_day_index(data.classrooms.days, a.day);
        int num_periods = (int)data.classrooms.periods.size();
        
        if (start_period_idx >= 0 && day_idx >= 0) {
            // Look up teacher's time preferences from the map
            auto teacher_it = time_pref_map.find(a.teacher_id);
            if (teacher_it != time_pref_map.end()) {
                for (int t = 0; t < required_periods && start_period_idx + t < num_periods; ++t) {
                    int period_idx = start_period_idx + t;
                    int slot_idx = slot_index(day_idx, period_idx, num_periods);
                    
                    // Look up preference score for this slot
                    auto slot_it = teacher_it->second.find(slot_idx);
                    if (slot_it != teacher_it->second.end()) {
                        score += slot_it->second;
                    }
                }
            }
        }
        return score;
    }
    
    // Compute stability penalty between current and initial assignment
    static double compute_stability_penalty(const OptimalSolution::Assignment &current, 
                                           const OptimalSolution::Assignment &initial, 
                                           const ProblemData &data) {
        double penalty = 0.0;
        
        // Penalty for teacher change
        if (current.teacher_id != initial.teacher_id) {
            penalty += STABILITY_TEACHER_PENALTY;
        }
        
        // Penalty for timeslot change
        if (current.day != initial.day || current.period != initial.period) {
            penalty += STABILITY_TIMESLOT_PENALTY;
        }
        
        // Core penalty multiplier if both teacher and timeslot changed
        if (current.teacher_id != initial.teacher_id && 
            (current.day != initial.day || current.period != initial.period)) {
            penalty *= STABILITY_CORE_MULTIPLIER;
        }
        
        return penalty;
    }
    
    // Compute workload variance delta from assignment changes
    static double compute_workload_delta(const vector<AssignmentChange> &changes, 
                                        const PenaltyState &current_state, 
                                        const ProblemData &data) {
        // Create a temporary state to compute new workload variance
        PenaltyState temp_state = current_state;
        
        // Apply all changes to temporary state (only workload, not compactness)
        for (const auto &chg : changes) {
            temp_state.update_workload(chg, data);
        }
        
        // Return the difference in variance
        return temp_state.workload_var - current_state.workload_var;
    }
    
    // Compute compactness delta from assignment changes
    // Note: This function modifies day_slots, so it must work on a copy of the state
    static double compute_compactness_delta(const vector<AssignmentChange> &changes, 
                                           const PenaltyState &current_state, 
                                           const ProblemData &data) {
        // Create a temporary state to compute new compactness
        // This copies all state including day_slots which is needed for update_compactness
        PenaltyState temp_state = current_state;
        
        // Store initial compactness
        double initial_compactness = current_state.compactness;
        
        // Apply all changes to temporary state (only compactness, not workload)
        for (const auto &chg : changes) {
            temp_state.update_compactness(chg, data);
        }
        
        // Return the difference in compactness
        return temp_state.compactness - initial_compactness;
    }

    static double Evaluate(const OptimalSolution &sol, const ProblemData &data,
                        const unordered_map<string, const Teacher*> &teacher_map,
                        const unordered_map<string, unordered_map<int, int>> &time_pref_map,
                        const OptimalSolution &initial_sol) {
        const double w_course_pref = 1.0, w_time_pref = 1.0, w_workload_balance = 5.0;
        const double w_compactness = 3.0, w_stability = 2.0;
        double course_pref_score = 0.0, time_pref_score = 0.0;
        for (const auto &a : sol.assignments) {
            auto it_teacher = teacher_map.find(a.teacher_id);
            if (it_teacher != teacher_map.end()) {
                auto pc_it = it_teacher->second->course_pref.find(a.course_id);
                if (pc_it != it_teacher->second->course_pref.end()) course_pref_score += pc_it->second;
            }
            time_pref_score += get_time_pref_score(a, 0, 0, data, time_pref_map);
        }
        PenaltyState state = init_penalty_state(sol, data);
        unordered_map<string, OptimalSolution::Assignment> initial_map;
        for (const auto &a : initial_sol.assignments) initial_map[get_assignment_id(a)] = a;
        double stability_penalty = 0.0;
        for (const auto &a : sol.assignments) {
            auto it = initial_map.find(get_assignment_id(a));
            if (it != initial_map.end()) stability_penalty += compute_stability_penalty(a, it->second, data);
        }
        return w_course_pref * course_pref_score + w_time_pref * time_pref_score
               - w_workload_balance * state.get_workload_penalty()
               - w_compactness * state.compactness
               - w_stability * stability_penalty;
    }
    
    class MoveEvaluator {
    private:
        const OptimalSolution &current;
        const SolIndex &current_idx;
        const ProblemData &data;
        
    public:
        MoveEvaluator(const OptimalSolution &sol, const SolIndex &idx, const ProblemData &d) 
            : current(sol), current_idx(idx), data(d) {}
        
        MoveContext evaluate(const MoveSpec &spec) {
            MoveContext ctx{false};
            ctx.candidate = current;
            ctx.move.type = spec.type;
            ctx.move.indices = spec.assignment_indices;
            
            if (spec.type == Move::SINGLE_CHANGE || spec.type == Move::BLOCK_RELOCATE) {
                // ------------------------------------------------------------------------
                // Single Change / Block Relocate Evaluation
                // ------------------------------------------------------------------------
                // Single change: Change teacher OR timeslot for one assignment
                // Block relocate: Move assignment to new timeslot (may change teacher if needed)
                //
                // Penalty updates:
                //   - Workload: Changes if teacher changes (periods move between teachers)
                //   - Compactness: Changes for both old and new teacher-day combinations
                //   - Classroom: May change due to new timeslot constraints
                
                if (spec.assignment_indices.empty()) return ctx;
                int assignment_idx = spec.assignment_indices[0];
                if (assignment_idx < 0 || assignment_idx >= (int)current.assignments.size()) return ctx;
                
                const auto &old_assignment = current.assignments[assignment_idx];
                OptimalSolution::Assignment new_assignment = old_assignment;
                
                // Determine what to change: teacher or timeslot
                if (!spec.new_teacher_id.empty() && spec.new_teacher_id != old_assignment.teacher_id) {
                    // Change teacher (single change)
                    new_assignment.teacher_id = spec.new_teacher_id;
                } else if (!spec.new_day.empty() && !spec.new_period.empty()) {
                    // Change timeslot (block relocate or single change)
                    new_assignment.day = spec.new_day;
                    new_assignment.period = spec.new_period;
                } else {
                    return ctx;  // No valid change specified
                }
                
                // Try to apply the change: checks feasibility and assigns classroom
                TryAssignmentResult assignment_result = try_assignment_change(
                    old_assignment, new_assignment, current_idx, data);
                
                if (!assignment_result.ok) return ctx;  // Change not feasible
                
                // Change is feasible: update solution
                ctx.idx_after = current_idx;
                apply_index_delta(ctx.idx_after, assignment_result.delta);  // Update index with delta
                
                // Update assignment with new values and assigned classroom
                new_assignment.classroom_id = assignment_result.classroom_id;
                ctx.candidate.assignments[assignment_idx] = new_assignment;
                
                ctx.ok = true;
                
            } else if (spec.type == Move::CHAIN_MOVE) {
                // ------------------------------------------------------------------------
                // Chain Move Evaluation
                // ------------------------------------------------------------------------
                // Chain moves rotate periods of consecutive assignments for the same teacher.
                // Each assignment in the chain gets a new period (from rotation).
                // We need to:
                //   1. Validate all chain blocks
                //   2. Try each assignment change sequentially (updating index incrementally)
                //   3. Track combined delta for all changes
                //   4. Update classroom assignments (may change due to period change)
                //
                // Penalty updates:
                //   - Workload: Unchanged (same teacher, same total periods per assignment)
                //   - Compactness: Changes based on new period distribution
                //   - Teacher/classroom: Teacher unchanged, classroom may change
                
                if (spec.chain.empty()) return ctx;
                
                // Validate all chain block indices
                for (const auto &chain_block : spec.chain) {
                    if (chain_block.assignment_idx < 0 || chain_block.assignment_idx >= (int)current.assignments.size()) {
                        return ctx;  // Invalid block index
                    }
                }
                
                // Evaluate chain move: process each block sequentially
                // We use a trial index that gets updated incrementally to track feasibility
                SolIndex trial_solution_index = current_idx;
                vector<OptimalSolution::Assignment> updated_assignments;
                IndexDelta combined_index_delta;  // Accumulated delta for all chain blocks
                
                // Process each block in the chain
                for (const auto &chain_block : spec.chain) {
                    const auto &old_assignment = current.assignments[chain_block.assignment_idx];
                    OptimalSolution::Assignment new_assignment = old_assignment;
                    
                    // Update period from chain rotation (only period changes in chain move)
                    new_assignment.period = chain_block.period;
                    
                    // Try to apply this assignment change (checks feasibility and updates classroom)
                    TryAssignmentResult assignment_result = try_assignment_change(
                        old_assignment, new_assignment, trial_solution_index, data);
                    
                    if (!assignment_result.ok) {
                        return ctx;  // Chain move infeasible if any block fails
                    }
                    
                    // Update assignment with new classroom (may change due to period change)
                    new_assignment.classroom_id = assignment_result.classroom_id;
                    updated_assignments.push_back(new_assignment);
                    
                    // Accumulate index delta for this block
                    combined_index_delta += assignment_result.delta;
                    
                    // Update trial index incrementally (for next block's feasibility check)
                    apply_index_delta(trial_solution_index, assignment_result.delta);
                }
                
                // All blocks feasible: commit the chain move
                ctx.candidate = current;
                for (size_t block_idx = 0; block_idx < spec.chain.size(); ++block_idx) {
                    int assignment_idx = spec.chain[block_idx].assignment_idx;
                    ctx.candidate.assignments[assignment_idx] = updated_assignments[block_idx];
                }
                
                ctx.move.chain = spec.chain;
                ctx.idx_after = current_idx;
                apply_index_delta(ctx.idx_after, combined_index_delta);  // Apply all deltas at once
                ctx.ok = true;
            }
            
            if (ctx.ok) ctx.changes = expand_move(ctx.move, current, ctx.candidate);
            return ctx;
        }
    };
    
    // ----------------------------------------------------------------------------
    // Move Expansion: Convert Move to Assignment Changes
    // ----------------------------------------------------------------------------
    
    // Expand a move into a list of assignment changes for penalty calculation
    // This is used to compute penalty deltas (workload, compactness) after a move
    //
    // For chain moves: Returns changes for all assignments in the chain (period changes)
    // For block/single moves: Returns change for the single affected assignment
    //
    // Only includes assignments that actually changed (teacher, day, period, or classroom)
    static vector<AssignmentChange> expand_move(const Move &move, 
                                                const OptimalSolution &current_solution, 
                                                const OptimalSolution &candidate_solution) {
        vector<int> affected_assignment_indices = move.indices;
        vector<AssignmentChange> assignment_changes;
        
        for (int assignment_idx : affected_assignment_indices) {
            // Validate index bounds
            if (assignment_idx < 0 || 
                assignment_idx >= (int)current_solution.assignments.size() || 
                assignment_idx >= (int)candidate_solution.assignments.size()) {
                continue;
            }
            
            const auto &old_assignment = current_solution.assignments[assignment_idx];
            const auto &new_assignment = candidate_solution.assignments[assignment_idx];
            
            // Check if assignment actually changed (any field)
            bool assignment_changed = (old_assignment.teacher_id != new_assignment.teacher_id ||
                                      old_assignment.day != new_assignment.day ||
                                      old_assignment.period != new_assignment.period ||
                                      old_assignment.classroom_id != new_assignment.classroom_id);
            
            if (assignment_changed) {
                assignment_changes.push_back({assignment_idx, old_assignment, new_assignment});
            }
        }
        
        return assignment_changes;
    }
    
    // ----------------------------------------------------------------------------
    // Penalty State Update After Move
    // ----------------------------------------------------------------------------
    
    // Update penalty state after a move has been accepted
    // This updates workload and compactness penalties based on assignment changes
    //
    // For chain moves:
    //   - Workload: Unchanged (same teacher, same periods per assignment)
    //   - Compactness: Updated based on new period distribution (gaps may change)
    //
    // For block/single moves:
    //   - Workload: Updated if teacher changes (periods move between teachers)
    //   - Compactness: Updated for both old and new teacher-day combinations
    static void update_penalty_state(const MoveContext &move_context, 
                                     PenaltyState &penalty_state, 
                                     const MoveDelta &move_delta,
                                     const ProblemData &data) {
        // Apply each assignment change to update workload and compactness
        // PenaltyState.apply_change() handles:
        //   1. Workload updates (teacher period counts)
        //   2. Compactness updates (period gaps per teacher-day)
        for (const auto &assignment_change : move_context.changes) {
            penalty_state.apply_change(assignment_change, data);
        }
    }
    
    static pair<double, double> evaluate_move(const MoveContext &ctx, const OptimalSolution &current,
                                             const PenaltyState &current_state,
                                             const ProblemData &data, const unordered_map<string, const Teacher*> &teacher_map,
                                             const unordered_map<string, unordered_map<int, int>> &time_pref_map,
                                             const unordered_map<string, OptimalSolution::Assignment> &initial_map,
                                             const OptimalSolution &initial_sol,
                                             MoveDelta &delta_out) {
        const double w_course_pref = 1.0, w_time_pref = 1.0, w_workload_balance = 5.0;
        const double w_compactness = 3.0, w_stability = 2.0;
        
        double delta_course_pref = 0.0, delta_time_pref = 0.0, delta_stability = 0.0;
        for (const auto &chg : ctx.changes) {
            auto get_course_pref = [&](const string &tid, const string &cid) -> double {
                auto it = teacher_map.find(tid);
                if (it == teacher_map.end()) return 0.0;
                auto pc_it = it->second->course_pref.find(cid);
                return (pc_it != it->second->course_pref.end()) ? pc_it->second : 0.0;
            };
            delta_course_pref += get_course_pref(chg.new_a.teacher_id, chg.new_a.course_id) - 
                                 get_course_pref(chg.old_a.teacher_id, chg.old_a.course_id);
            delta_time_pref += get_time_pref_score(chg.new_a, 0, 0, data, time_pref_map) -
                               get_time_pref_score(chg.old_a, 0, 0, data, time_pref_map);
            auto it_initial = initial_map.find(get_assignment_id(chg.old_a));
            if (it_initial != initial_map.end())
                delta_stability += compute_stability_penalty(chg.new_a, it_initial->second, data) - 
                                   compute_stability_penalty(chg.old_a, it_initial->second, data);
        }
        
        delta_out.delta_workload_var = compute_workload_delta(ctx.changes, current_state, data);
        delta_out.delta_compactness = compute_compactness_delta(ctx.changes, current_state, data);
        
        double current_workload_var = current_state.workload_var;
        double new_workload_var = current_workload_var + delta_out.delta_workload_var;
        int current_num_teachers = (int)current_state.workload.size();
        
        unordered_map<string, int> new_workload = current_state.workload;
        for (const auto &chg : ctx.changes) {
            int old_p = get_required_periods(data, chg.old_a.course_id, chg.old_a.section_id);
            int new_p = get_required_periods(data, chg.new_a.course_id, chg.new_a.section_id);
            new_workload[chg.old_a.teacher_id] -= old_p;
            if (new_workload[chg.old_a.teacher_id] <= 0) new_workload.erase(chg.old_a.teacher_id);
            new_workload[chg.new_a.teacher_id] += new_p;
        }
        int new_num_teachers = (int)new_workload.size();
        
        double current_workload_penalty = (current_num_teachers > 0) ? sqrt(current_workload_var / current_num_teachers) : 0.0;
        double new_workload_penalty = (new_num_teachers > 0) ? sqrt(new_workload_var / new_num_teachers) : 0.0;
        double delta_workload = new_workload_penalty - current_workload_penalty;
        
        double delta_soft = w_course_pref * delta_course_pref + w_time_pref * delta_time_pref - w_stability * delta_stability;
        double delta = delta_soft - w_workload_balance * delta_workload - w_compactness * delta_out.delta_compactness;
        
        delta_out.delta_soft_local = delta_soft;
        delta_out.delta_hard = 0;
        
        return {current.objective_value + delta, delta};
    }

    // ----------------------------------------------------------------------------
    // Move Acceptance with Tabu and SA
    // ----------------------------------------------------------------------------
    
    // Check if a move should be accepted considering both Tabu Search and Simulated Annealing
    // Returns AcceptMoveResult with acceptance decision and rejection reasons
    static AcceptMoveResult accept_move(const Move &move, 
                                       const OptimalSolution &current_solution, 
                                       const OptimalSolution &candidate_solution,
                                       double candidate_score, 
                                       double objective_delta, 
                                       double temperature, 
                                       SimpleTabu &tabu_list, 
                                       double best_score,
                                       int &tabu_rejected_count, 
                                       int &sa_rejected_count) {
        AcceptMoveResult result{false, false, false, "", "", ""};
        
        // Extract tabu signatures for this move
        auto tabu_keys = extract_tabu_keys(current_solution, candidate_solution, move);
        result.sig_teacher = get<0>(tabu_keys);
        result.sig_timeslot = get<1>(tabu_keys);
        result.sig_chain_teacher_day = get<2>(tabu_keys);
        
        // Check if move is tabu
        bool is_tabu_move = tabu_list.is_tabu(result.sig_teacher, result.sig_timeslot, result.sig_chain_teacher_day);
        
        // Aspiration criterion: override tabu if move improves best solution
        bool aspiration_criterion = candidate_score > best_score;
        
        // Reject if tabu and no aspiration
        if (is_tabu_move && !aspiration_criterion) {
            result.rejected_by_tabu = true;
            tabu_rejected_count++;
            return result;
        }
        
        // Check if current solution is at best (for SA escape mechanism)
        const double BEST_TOLERANCE = 0.01;  // Consider at best if within this tolerance
        bool is_current_at_best = (current_solution.objective_value >= best_score - BEST_TOLERANCE);
        
        // Simulated Annealing acceptance decision
        bool sa_accepted = should_accept_move(objective_delta, temperature, sa_rejected_count, 
                                             best_score, candidate_score, is_current_at_best);
        
        if (sa_accepted) {
            // Move accepted: add to tabu list and mark as accepted
            tabu_list.add_tabu(result.sig_teacher, result.sig_timeslot, result.sig_chain_teacher_day);
            result.accepted = true;
        } else {
            // Move rejected by SA
            result.rejected_by_sa = true;
        }
        
        return result;
    }
    
    // ----------------------------------------------------------------------------
    // Solution Update After Move Acceptance
    // ----------------------------------------------------------------------------
    
    // Commit an accepted move to update current and best solutions
    // Updates solution index, current solution, and tracks best solution
    static void commit_move(const MoveContext &move_context, 
                           double candidate_score,
                           OptimalSolution &current_solution, 
                           OptimalSolution &best_solution, 
                           SolIndex &current_solution_index,
                           int &iterations_without_improvement) {
        // Update solution index to reflect the move
        current_solution_index = move_context.idx_after;
        
        // Update current solution
        current_solution = move_context.candidate;
        current_solution.objective_value = candidate_score;
        
        // Check if this is a new best solution
        if (candidate_score > best_solution.objective_value) {
            cout << "[Phase3] NEW BEST: " << candidate_score << " (was " << best_solution.objective_value << ")\n";
            best_solution = move_context.candidate;
            best_solution.objective_value = candidate_score;
            iterations_without_improvement = 0;  // Reset counter on improvement
        } else {
            iterations_without_improvement++;  // Increment counter if no improvement
        }
    }
    
    static inline void update_ema_weight(int nb, double reward, double beta, double w_min, double w_max,
                                  vector<double> &nb_score, vector<double> &nb_weight) {
        nb_score[nb] = beta * reward + (1.0 - beta) * nb_score[nb];
        nb_weight[nb] = w_min + (w_max - w_min) * nb_score[nb];
    }
}

// ============================================================================
// PHASE 3 MAIN FUNCTION - Initialization, Execution, and Output
// ============================================================================

// Structure to hold all initialization data for Phase 3 search
struct Phase3InitData {
    unordered_map<string, const Teacher*> teacher_map;
    unordered_map<string, unordered_map<int, int>> time_pref_map;
    unordered_map<string, OptimalSolution::Assignment> initial_assignment_map;
    OptimalSolution initial_solution;
    OptimalSolution current_solution;
    OptimalSolution best_solution;
    double initial_score;
    SimpleTabu tabu_list;
    SolIndex solution_index;
    PenaltyState penalty_state;
    
    // Adaptive search parameters
    const int num_neighborhoods = 3;
    const double ema_beta = 0.1;
    const double weight_min = 0.2;
    const double weight_max = 3.0;
    vector<double> neighborhood_scores;
    vector<double> neighborhood_weights;
    
    // Simulated Annealing parameters
    double temperature;
    const double initial_temperature = 1.0;
    const double cooling_rate = 0.98;
    
    // Search control parameters
    const int max_iterations = 1200;
    const int base_moves_per_neighborhood = 32;
    const int limit_no_improvement = 220;
    const int stuck_threshold = 100;
    const int intensification_interval = 50;
    
    // Statistics
    int iterations_without_improvement = 0;
    int total_feasible_moves = 0;
    int accepted_moves = 0;
    int tabu_rejected_count = 0;
    int sa_rejected_count = 0;
    int infeasible_moves_count = 0;
    int stuck_iterations_count = 0;
    int last_intensification_iteration;
    bool just_improved_best_flag = false;
    double last_best_score_value;
};

// ----------------------------------------------------------------------------
// Phase 3 Initialization
// ----------------------------------------------------------------------------

// Initialize all data structures, maps, and solutions for Phase 3 search
// Performance: O(N) for building maps and indices, O(1) for penalty state initialization
// Returns: Phase3InitData with all initialized structures
static Phase3InitData initialize_phase3_search(const ProblemData &data, const InitialSolution &initial) {
    Phase3InitData init_data;
    
    // Build teacher lookup map (O(T) where T = number of teachers)
    for (const auto &teacher : data.teachers) {
        init_data.teacher_map[teacher.id] = &teacher;
    }
    
    // Build time preference map: teacher -> slot_index -> preference_score
    // This allows O(1) lookup of time preferences during evaluation
    int num_periods = (int)data.classrooms.periods.size();
    for (const auto &teacher : data.teachers) {
        for (const auto &time_pref : teacher.time_pref) {
            int day_idx = find_day_index(data.classrooms.days, time_pref.day);
            int period_idx = find_period_index(data.classrooms.periods, time_pref.period);
            if (day_idx >= 0 && period_idx >= 0) {
                int slot_idx = slot_index(day_idx, period_idx, num_periods);
                init_data.time_pref_map[teacher.id][slot_idx] = time_pref.score;
            }
        }
    }
    
    // Initialize solutions from initial solution
    init_data.initial_solution = initial;
    init_data.current_solution = initial;
    init_data.best_solution = initial;
    
    // Evaluate initial solution and set objective values
    init_data.initial_score = Evaluate(init_data.initial_solution, data, 
                                       init_data.teacher_map, init_data.time_pref_map, 
                                       init_data.initial_solution);
    init_data.initial_solution.objective_value = init_data.initial_score;
    init_data.current_solution.objective_value = init_data.initial_score;
    init_data.best_solution.objective_value = init_data.initial_score;
    
    // Initialize Tabu Search list
    init_data.tabu_list = SimpleTabu(DEFAULT_TABU_TENURE);
    
    // Build solution index for fast constraint checking (O(N) where N = assignments)
    // This index allows O(1) feasibility checks instead of O(N) linear search
    init_data.solution_index = build_index(init_data.current_solution, data);
    
    // Initialize penalty state with O(1) update capability
    // Uses sum and sum_squared for O(1) variance updates instead of O(T) recalculation
    init_data.penalty_state = init_penalty_state(init_data.current_solution, data);
    
    // Build initial assignment map for stability penalty calculation
    for (const auto &assignment : init_data.initial_solution.assignments) {
        string assignment_id = get_assignment_id(assignment);
        init_data.initial_assignment_map[assignment_id] = assignment;
    }
    
    // Initialize adaptive neighborhood weights
    // Neighborhoods: 0 = single change, 1 = block relocate, 2 = chain move
    init_data.neighborhood_scores = {
        (1.2 - init_data.weight_min) / (init_data.weight_max - init_data.weight_min),
        (0.8 - init_data.weight_min) / (init_data.weight_max - init_data.weight_min),
        (0.5 - init_data.weight_min) / (init_data.weight_max - init_data.weight_min)
    };
    init_data.neighborhood_weights.resize(init_data.num_neighborhoods);
    for (int i = 0; i < init_data.num_neighborhoods; ++i) {
        init_data.neighborhood_weights[i] = init_data.weight_min + 
            (init_data.weight_max - init_data.weight_min) * init_data.neighborhood_scores[i];
    }
    
    // Initialize Simulated Annealing temperature
    init_data.temperature = init_data.initial_temperature;
    init_data.last_best_score_value = init_data.best_solution.objective_value;
    init_data.last_intensification_iteration = -init_data.intensification_interval;
    
    return init_data;
}

// ----------------------------------------------------------------------------
// Debug Logging Functions
// ----------------------------------------------------------------------------

// Log iteration progress (called periodically during SA loop)
static void log_iteration_progress(int iteration, 
                                   double best_score, 
                                   double current_score, 
                                   double temperature,
                                   int iterations_without_improvement,
                                   int stuck_iterations) {
    const int LOG_INTERVAL = 50;  // Log every N iterations
    if (iteration % LOG_INTERVAL == 0) {
        cout << "[Phase3] Iter " << iteration 
             << " | Best=" << best_score 
             << " | Current=" << current_score 
             << " | T=" << temperature 
             << " | NoImprov=" << iterations_without_improvement 
             << " | Stuck=" << stuck_iterations << "\n";
    }
}

// Log final results and statistics
static void log_phase3_results(const Phase3InitData &init_data,
                               double final_temperature,
                               int total_iterations) {
    double improvement = init_data.best_solution.objective_value - init_data.initial_score;
    double improvement_percentage = (init_data.initial_score != 0.0) 
        ? (improvement / abs(init_data.initial_score)) * 100.0 
        : 0.0;
    
    cout << "\n[Phase3] ========== FINAL RESULTS ==========\n";
    cout << "[Phase3] Best Objective Value: " << init_data.best_solution.objective_value << "\n";
    cout << "[Phase3] Initial Objective Value: " << init_data.initial_score << "\n";
    cout << "[Phase3] Improvement: " << improvement << " (" << improvement_percentage << "%)\n";
    cout << "[Phase3] Total Iterations: " << total_iterations << "\n";
    cout << "[Phase3] Final Temperature: " << final_temperature << "\n";
    cout << "[Phase3] Accepted Moves: " << init_data.accepted_moves << "\n";
    cout << "[Phase3] Total Feasible Moves: " << init_data.total_feasible_moves << "\n";
    cout << "[Phase3] Tabu Rejected: " << init_data.tabu_rejected_count << "\n";
    cout << "[Phase3] SA Rejected: " << init_data.sa_rejected_count << "\n";
    cout << "[Phase3] Infeasible Moves: " << init_data.infeasible_moves_count << "\n";
    cout << "[Phase3] Total Assignments: " << init_data.best_solution.assignments.size() << "\n";
    cout << "[Phase3] ====================================\n\n";
}

// ----------------------------------------------------------------------------
// Main Simulated Annealing Execution
// ----------------------------------------------------------------------------

// Run the main Simulated Annealing loop with Tabu Search
// Flow: For each iteration -> Generate moves -> Evaluate -> Accept/Reject -> Update
// Performance: Uses O(1) penalty updates and cached indices for fast evaluation
static void run_simulated_annealing(Phase3InitData &init_data,
                                   const ProblemData &data,
                                   const OptimalSolution &initial_solution) {

    // ============================================================================
    // SIMULATED ANNEALING MAIN LOOP
    // ============================================================================
    // Flow: For each iteration:
    //   1. Check temperature and termination conditions
    //   2. Log progress (if needed)
    //   3. Check for best improvement
    //   4. Intensification (if needed)
    //   5. Generate and evaluate moves (neighborhood search)
    //   6. Shake (if no improvement)
    //   7. Update state (penalties, temperature, weights)
    //   8. Restart (if stuck)
    // ============================================================================
    
    for (int iteration = 0; iteration < init_data.max_iterations; ++iteration) {
        // ------------------------------------------------------------------------
        // Temperature Control and Termination Check
        // ------------------------------------------------------------------------
        
        // Check if temperature has cooled below minimum
        if (init_data.temperature < MIN_TEMPERATURE) {
            // Reheat if stuck but not near end of iterations (allows more exploration)
            const int REHEAT_BUFFER = 50;  // Don't reheat in last N iterations
            if (iteration < init_data.max_iterations - REHEAT_BUFFER && 
                init_data.iterations_without_improvement > init_data.stuck_threshold) {
                const double REHEAT_FACTOR = 0.5;  // Reheat to 50% of initial temperature
                init_data.temperature = init_data.initial_temperature * REHEAT_FACTOR;
                init_data.stuck_iterations_count = 0;
                cout << "[Phase3] Reheating: T=" << init_data.temperature << " at iter " << iteration << "\n";
            } else {
                // Temperature too low and can't reheat, terminate
                break;
            }
        }
        
        // ------------------------------------------------------------------------
        // Iteration Logging
        // ------------------------------------------------------------------------
        
        log_iteration_progress(iteration, 
                              init_data.best_solution.objective_value,
                              init_data.current_solution.objective_value,
                              init_data.temperature,
                              init_data.iterations_without_improvement,
                              init_data.stuck_iterations_count);
        
        // Check if we just improved best solution
        if (init_data.best_solution.objective_value > init_data.last_best_score_value) {
            init_data.just_improved_best_flag = true;
            init_data.last_best_score_value = init_data.best_solution.objective_value;
            init_data.stuck_iterations_count = 0;  // Reset stuck counter
        } else {
            init_data.just_improved_best_flag = false;
        }
        
        // ------------------------------------------------------------------------
        // Intensification: Search Around Best Solution
        // ------------------------------------------------------------------------
        // Periodically search around best solution to find local improvements
        // More frequent when temperature is very low or when we just improved best
        bool intensification_mode = false;
        int intensification_interval = init_data.intensification_interval;
        if (init_data.temperature < MIN_TEMPERATURE * 100) {
            intensification_interval = 10;  // Every 10 iterations when T very low
        } else if (init_data.temperature < init_data.initial_temperature * 0.1) {
            intensification_interval = 20;  // Every 20 iterations when T low
        }
        
        if (init_data.just_improved_best_flag || 
            (iteration - init_data.last_intensification_iteration >= intensification_interval && 
             init_data.temperature < init_data.initial_temperature * 0.3)) {
            intensification_mode = true;
            init_data.current_solution = init_data.best_solution;  // Set current to best for intensification
            init_data.solution_index = build_index(init_data.current_solution, data);
            init_data.penalty_state = init_penalty_state(init_data.current_solution, data);
            init_data.last_intensification_iteration = iteration;
            if (init_data.just_improved_best_flag) {
                cout << "[Phase3] Intensification: new best found, searching around it\n";
            } else {
                cout << "[Phase3] Intensification: searching around best solution\n";
            }
        }
        
        // ------------------------------------------------------------------------
        // Adaptive Move Generation: Adjust moves per neighborhood based on state
        // ------------------------------------------------------------------------
        // Increase move attempts when:
        //   - Temperature is low (need more exploration)
        //   - Search is stuck (need diversification)
        //   - Just improved best (intensify around good solution)
        int moves_per_neighborhood = init_data.base_moves_per_neighborhood;
        if (init_data.temperature < MIN_TEMPERATURE * 100) {
            moves_per_neighborhood = init_data.base_moves_per_neighborhood * 3;  // Triple when T very low
        } else if (init_data.temperature < init_data.initial_temperature * 0.1) {
            moves_per_neighborhood = (int)(init_data.base_moves_per_neighborhood * 2);  // Double when T low
        }
        if (init_data.stuck_iterations_count > init_data.stuck_threshold / 2) {
            moves_per_neighborhood = max(moves_per_neighborhood, init_data.base_moves_per_neighborhood * 2);
        }
        if (init_data.just_improved_best_flag || intensification_mode) {
            moves_per_neighborhood = max(moves_per_neighborhood, init_data.base_moves_per_neighborhood * 2);
        }
        
        // ------------------------------------------------------------------------
        // Neighborhood Search: Try moves from each neighborhood
        // ------------------------------------------------------------------------
        // Neighborhoods are ordered by weight (highest first) for adaptive search
        bool any_improved = false;
        vector<int> neighborhood_order(init_data.num_neighborhoods);
        iota(neighborhood_order.begin(), neighborhood_order.end(), 0);
        sort(neighborhood_order.begin(), neighborhood_order.end(), 
             [&](int a, int b){ 
                 return init_data.neighborhood_weights[a] > init_data.neighborhood_weights[b]; 
             });
        
        for (int ordered_idx = 0; ordered_idx < init_data.num_neighborhoods; ++ordered_idx) {
            int neighborhood_idx = neighborhood_order[ordered_idx];
            bool improved_in_neighborhood = false;
            
            // Try moves from this neighborhood
            for (int move_attempt = 0; move_attempt < moves_per_neighborhood; ++move_attempt) {
                // Generate move based on neighborhood type
                MoveBuilder move_builder(init_data.current_solution, data);
                MoveSpec move_spec;
                bool move_generated = false;
                
                if (neighborhood_idx == 2) {
                    // Neighborhood 2: Chain moves (rotate consecutive assignments)
                    move_generated = move_builder.build_chain_move(init_data.penalty_state);
                } else if (neighborhood_idx == 0) {
                    // Neighborhood 0: Single changes (change teacher or timeslot)
                    move_generated = move_builder.build_single_change();
                } else {
                    // Neighborhood 1: Block relocations (move assignment to new timeslot)
                    move_generated = move_builder.build_block_relocate();
                }
                
                if (move_generated) {
                    move_spec = move_builder.get_spec();
                }
                if (!move_generated) { 
                    init_data.infeasible_moves_count++; 
                    continue; 
                }
                
                // Evaluate move feasibility
                MoveEvaluator move_evaluator(init_data.current_solution, init_data.solution_index, data);
                MoveContext move_context = move_evaluator.evaluate(move_spec);
                if (!move_context.ok) { 
                    init_data.infeasible_moves_count++; 
                    continue; 
                }
                
                init_data.total_feasible_moves++;
                
                // Evaluate move quality (compute objective delta)
                MoveDelta move_delta;
                auto [candidate_score, objective_delta] = evaluate_move(
                    move_context, init_data.current_solution, init_data.penalty_state, data,
                    init_data.teacher_map, init_data.time_pref_map, 
                    init_data.initial_assignment_map, init_data.initial_solution, move_delta);
                
                // Skip invalid moves (except chain moves which may have zero delta)
                if (candidate_score == 0.0 && objective_delta == 0.0 && move_context.move.type != Move::CHAIN_MOVE) {
                    init_data.infeasible_moves_count++;
                    continue;
                }
                
                // ------------------------------------------------------------------------
                // Simulated Annealing: Check Move Acceptance
                // ------------------------------------------------------------------------
                
                AcceptMoveResult acceptance_result = accept_move(
                    move_context.move, init_data.current_solution, move_context.candidate, 
                    candidate_score, objective_delta, init_data.temperature, init_data.tabu_list, 
                    init_data.best_solution.objective_value,
                    init_data.tabu_rejected_count, init_data.sa_rejected_count);
                
                if (!acceptance_result.accepted) continue;  // Move rejected, try next move
                
                // ------------------------------------------------------------------------
                // Move Accepted: Update Solutions and State
                // ------------------------------------------------------------------------
                
                init_data.accepted_moves++;
                
                // Update neighborhood weights based on move quality (adaptive search)
                // Higher reward for moves that improve best solution
                double move_reward = 0.0;
                if (candidate_score > init_data.best_solution.objective_value) {
                    move_reward = 1.0;  // Full reward for improving best
                } else if (candidate_score > init_data.current_solution.objective_value) {
                    move_reward = 0.7;  // Partial reward for improving current
                }
                if (move_reward > 0.0) {
                    update_ema_weight(neighborhood_idx, move_reward, init_data.ema_beta, 
                                    init_data.weight_min, init_data.weight_max, 
                                    init_data.neighborhood_scores, init_data.neighborhood_weights);
                }
                
                // Commit the accepted move to update solutions
                // This updates solution index (O(1) via delta) and current/best solutions
                commit_move(move_context, candidate_score, 
                           init_data.current_solution, init_data.best_solution, 
                           init_data.solution_index, init_data.iterations_without_improvement);
                
                // Update penalty state using O(1) delta updates
                // PenaltyState.apply_change() uses cached sums for O(1) variance updates
                update_penalty_state(move_context, init_data.penalty_state, move_delta, data);
                
                improved_in_neighborhood = true;
                any_improved = true;
                init_data.stuck_iterations_count = 0;  // Reset stuck counter on improvement
                break;  // Move accepted, exit neighborhood search
            } 
            
            // Update neighborhood weight if no improvement (penalty)
            if (!improved_in_neighborhood) {
                update_ema_weight(neighborhood_idx, 0.0, init_data.ema_beta, 
                                init_data.weight_min, init_data.weight_max, 
                                init_data.neighborhood_scores, init_data.neighborhood_weights);
            }
        }

        // ------------------------------------------------------------------------
        // Shake: Diversification When No Improvement
        // ------------------------------------------------------------------------
        // If no improvement in neighborhood search, try aggressive shake moves
        // Shake is more aggressive when temperature is low, stuck, or at best solution
        if (!any_improved) {
            // Calculate number of shake attempts based on search state
            int base_shake_count = (init_data.temperature > 0.1) ? 4 : max(4, (int)(8 * init_data.temperature / 0.1));
            int shake_count = base_shake_count;
            
            if (init_data.stuck_iterations_count > init_data.stuck_threshold / 2) {
                shake_count = max(shake_count, 10);  // More shakes when stuck
            }
            if (init_data.temperature < MIN_TEMPERATURE * 100) {
                shake_count = max(shake_count, 16);  // Even more when T very low
            }
            // When at best and stuck, be very aggressive
            if (init_data.current_solution.objective_value >= init_data.best_solution.objective_value && 
                init_data.iterations_without_improvement > 10) {
                shake_count = max(shake_count, 20);  // Very aggressive when at best and stuck
            }
            
            // Try different move types in shake (diversification strategies)
            vector<function<bool(MoveBuilder&)>> shake_strategies = {
                [](MoveBuilder& b) { return b.build_block_relocate(); },
                [](MoveBuilder& b) { return b.build_single_change(); },
                [](MoveBuilder& b) { return b.build_multi_swap(2); },
                [](MoveBuilder& b) { return b.build_multi_swap(3); }
            };
            
            for (int shake_idx = 0; shake_idx < shake_count; ++shake_idx) {
                MoveBuilder shake_builder(init_data.current_solution, data);
                MoveSpec shake_spec;
                
                // Rotate through different strategies for diversification
                int strategy_idx = shake_idx % shake_strategies.size();
                bool shake_generated = shake_strategies[strategy_idx](shake_builder);
                
                if (shake_generated) {
                    shake_spec = shake_builder.get_spec();
                }
                if (!shake_generated) continue;
                
                MoveEvaluator shake_evaluator(init_data.current_solution, init_data.solution_index, data);
                MoveContext shake_context = shake_evaluator.evaluate(shake_spec);
                if (!shake_context.ok) continue;
                
                MoveDelta shake_move_delta;
                auto [shake_candidate_score, shake_objective_delta] = evaluate_move(
                    shake_context, init_data.current_solution, init_data.penalty_state, data,
                    init_data.teacher_map, init_data.time_pref_map, 
                    init_data.initial_assignment_map, init_data.initial_solution, shake_move_delta);
                
                if (shake_candidate_score == 0.0 && shake_objective_delta == 0.0 && 
                    shake_context.move.type != Move::CHAIN_MOVE) {
                    continue;
                }
                
                // Simulated Annealing: Check Move Acceptance (shake phase)
                AcceptMoveResult shake_acceptance_result = accept_move(
                    shake_context.move, init_data.current_solution, shake_context.candidate,
                    shake_candidate_score, shake_objective_delta, init_data.temperature, 
                    init_data.tabu_list, init_data.best_solution.objective_value,
                    init_data.tabu_rejected_count, init_data.sa_rejected_count);
                
                if (!shake_acceptance_result.accepted) continue;  // Move rejected, try next shake
                
                // Move Accepted: Update Solutions and State
                commit_move(shake_context, shake_candidate_score, 
                           init_data.current_solution, init_data.best_solution, 
                           init_data.solution_index, init_data.iterations_without_improvement);
                update_penalty_state(shake_context, init_data.penalty_state, shake_move_delta, data);
                any_improved = true;  // Mark as improved, exit shake loop
                break;
            }
        }
        
        // ------------------------------------------------------------------------
        // Update Search State After Iteration
        // ------------------------------------------------------------------------
        
        // Track stuck iterations (no improvement in this iteration)
        if (!any_improved) {
            init_data.stuck_iterations_count++;
        } else {
            init_data.stuck_iterations_count = 0;
        }
        
        // Decay neighborhood weights (gradual reduction of adaptive weights)
        // This allows weights to gradually return to baseline over time
        const double NEIGHBORHOOD_DECAY_RATE = 0.9995;
        for (int i = 0; i < init_data.num_neighborhoods; ++i) {
            init_data.neighborhood_scores[i] *= NEIGHBORHOOD_DECAY_RATE;
            init_data.neighborhood_weights[i] = init_data.weight_min + 
                (init_data.weight_max - init_data.weight_min) * init_data.neighborhood_scores[i];
        }
        
        // ------------------------------------------------------------------------
        // Simulated Annealing: Temperature Update
        // ------------------------------------------------------------------------
        
        // Adaptive temperature decay: slower decay when stuck to allow more exploration
        const double STUCK_DECAY_FACTOR = 0.995;  // Slower decay factor when stuck
        const int STUCK_DECAY_THRESHOLD = init_data.stuck_threshold / 2;
        const double MIN_TEMP_FOR_STUCK_DECAY = MIN_TEMPERATURE * 10;
        
        if (init_data.stuck_iterations_count > STUCK_DECAY_THRESHOLD && 
            init_data.temperature > MIN_TEMP_FOR_STUCK_DECAY) {
            init_data.temperature *= STUCK_DECAY_FACTOR;  // Slower decay when stuck
        } else {
            init_data.temperature *= init_data.cooling_rate;  // Normal exponential cooling: T = T * alpha
        }
        
        // Reheat mechanism: if stuck for too long, increase temperature to escape
        const double REHEAT_TEMPERATURE_FACTOR = 0.3;  // Reheat to 30% of initial
        const double REHEAT_TEMPERATURE_THRESHOLD = init_data.initial_temperature * 0.1;
        if (init_data.stuck_iterations_count > init_data.stuck_threshold && 
            init_data.temperature < REHEAT_TEMPERATURE_THRESHOLD) {
            init_data.temperature = init_data.initial_temperature * REHEAT_TEMPERATURE_FACTOR;
            init_data.stuck_iterations_count = 0;
            cout << "[Phase3] Reheating due to stuck: T=" << init_data.temperature << " at iter " << iteration << "\n";
        }

        // ------------------------------------------------------------------------
        // Solution Restart: When No Improvement for Too Long
        // ------------------------------------------------------------------------
        
        if (init_data.iterations_without_improvement > init_data.limit_no_improvement) {
            // Restart from best solution
            init_data.current_solution = init_data.best_solution;
            init_data.iterations_without_improvement = 0;
            init_data.stuck_iterations_count = 0;
            
            // Reset temperature on restart to allow more exploration
            const double RESTART_TEMP_THRESHOLD = init_data.initial_temperature * 0.2;
            const double RESTART_TEMP_FACTOR = 0.3;
            if (init_data.temperature < RESTART_TEMP_THRESHOLD) {
                init_data.temperature = init_data.initial_temperature * RESTART_TEMP_FACTOR;
                cout << "[Phase3] Restart: resetting T=" << init_data.temperature << " at iter " << iteration << "\n";
            }
            
            // Rebuild indices and state for restart (O(N) but only on restart)
            init_data.solution_index = build_index(init_data.current_solution, data);
            init_data.penalty_state = init_penalty_state(init_data.current_solution, data);
            
            // Shuffle assignments for diversification
            shuffle(init_data.current_solution.assignments.begin(), 
                   init_data.current_solution.assignments.end(), rng);
            
            // Perform local shakes to perturb the solution
            const int MIN_LOCAL_SHAKES = 4;
            const int MAX_LOCAL_SHAKES = 40;
            const double TEMP_THRESHOLD_FOR_SHAKES = 0.1;
            int base_local_shakes = min(max(MIN_LOCAL_SHAKES, 
                (int)init_data.current_solution.assignments.size() / 16), MAX_LOCAL_SHAKES);
            int num_local_shakes = (init_data.temperature > TEMP_THRESHOLD_FOR_SHAKES) 
                ? base_local_shakes 
                : max(1, (int)(base_local_shakes * init_data.temperature / TEMP_THRESHOLD_FOR_SHAKES));
            
            for (int shake_idx = 0; shake_idx < num_local_shakes; ++shake_idx) {
                MoveBuilder restart_builder(init_data.current_solution, data);
                MoveSpec restart_spec;
                bool restart_generated = restart_builder.build_single_change();
                if (!restart_generated) {
                    restart_generated = restart_builder.build_block_relocate();
                }
                if (restart_generated) {
                    restart_spec = restart_builder.get_spec();
                }
                if (!restart_generated) continue;
                
                MoveEvaluator restart_evaluator(init_data.current_solution, init_data.solution_index, data);
                MoveContext restart_context = restart_evaluator.evaluate(restart_spec);
                if (!restart_context.ok) continue;
                
                // Apply shake move without SA acceptance (forced acceptance for diversification)
                init_data.solution_index = restart_context.idx_after;
                init_data.current_solution = restart_context.candidate;
            }
            
            // Re-evaluate and update state after shakes
            init_data.current_solution.objective_value = Evaluate(
                init_data.current_solution, data, init_data.teacher_map, 
                init_data.time_pref_map, init_data.initial_solution);
            init_data.penalty_state = init_penalty_state(init_data.current_solution, data);
        }
    }
}

// ----------------------------------------------------------------------------
// Phase 3 Main Function
// ----------------------------------------------------------------------------

// Main function for Phase 3 optimization using Simulated Annealing + Tabu Search
// Flow: Initialize → Run SA → Output Results
// Performance: Uses O(1) penalty updates and cached indices throughout
static OptimalSolution find_optimal_solution_original(const ProblemData &data, const InitialSolution &initial) {
    // ============================================================================
    // STEP 1: INITIALIZATION
    // ============================================================================
    // Build all lookup maps, initialize solutions, and set up search state
    // Performance: O(N + T) where N = assignments, T = teachers
    Phase3InitData init_data = initialize_phase3_search(data, initial);
    
    // ============================================================================
    // STEP 2: SIMULATED ANNEALING EXECUTION
    // ============================================================================
    // Run main SA loop with adaptive move generation and penalty tracking
    // Performance: O(1) penalty updates via cached sums, O(1) index updates via deltas
    run_simulated_annealing(init_data, data, init_data.initial_solution);
    
    // ============================================================================
    // STEP 3: OUTPUT RESULTS
    // ============================================================================
    // Log final statistics and return best solution found
    log_phase3_results(init_data, init_data.temperature, init_data.max_iterations);
    
    return init_data.best_solution;
}

OptimalSolution find_optimal_solution(const ProblemData &data, const InitialSolution &initial)
{
    return find_optimal_solution_original(data, initial);
}

double evaluate_initial_solution(const ProblemData& data, const InitialSolution& init_sol) {
    unordered_map<string, const Teacher*> teacher_map;
    for (const auto &t : data.teachers) teacher_map[t.id] = &t;
    unordered_map<string, unordered_map<int, int>> time_pref_map;
    int num_periods = (int)data.classrooms.periods.size();
    for (const auto &t : data.teachers) {
        for (const auto &tp : t.time_pref) {
            int day_idx = find_day_index(data.classrooms.days, tp.day);
            int period_idx = find_period_index(data.classrooms.periods, tp.period);
            if (day_idx >= 0 && period_idx >= 0) {
                int slot_idx = slot_index(day_idx, period_idx, num_periods);
                time_pref_map[t.id][slot_idx] = tp.score;
            }
        }
    }
    return Evaluate(OptimalSolution(init_sol), data, teacher_map, time_pref_map, OptimalSolution(init_sol));
}
