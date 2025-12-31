#include "phase3_penalty_state.h"
#include "phase_common.h"
#include <cmath>
#include <algorithm>
#include <iostream>

namespace phase3 {

struct Slot {
    int day_idx;
    int period_idx;
};

template<typename F>
static inline void for_each_slot(const OptimalSolution::Assignment &a, const ProblemData &data, F fn) {
    int day_idx = find_day_index(data.classrooms.days, a.day);
    int period_idx = find_period_index(data.classrooms.periods, a.period);
    int required_periods = get_required_periods(data, a.course_id, a.section_id);
    
    if (day_idx < 0 || day_idx >= (int)data.classrooms.days.size() || period_idx < 0) return;
    if (period_idx >= MAX_PERIOD_INDEX || period_idx + required_periods > MAX_PERIOD_INDEX) {
        return;
    }
    if (period_idx + required_periods > (int)data.classrooms.periods.size()) return;
    
    for (int i = 0; i < required_periods; ++i) {
        int slot_period_idx = period_idx + i;
        if (slot_period_idx >= 0 && slot_period_idx < MAX_PERIOD_INDEX) {
            fn(Slot{day_idx, slot_period_idx});
        }
    }
}

double PenaltyState::get_workload_penalty() const {
    int num_teachers = (int)workload.size();
    return (num_teachers > 0) ? std::sqrt(workload_var / num_teachers) : 0.0;
}

void PenaltyState::update_workload_var_from_sums() {
    int num_teachers = (int)workload.size();
    if (num_teachers > 0) {
        double mean = sum_workload / num_teachers;
        workload_var = (sum_workload_squared / num_teachers) - (mean * mean);
        if (workload_var < 0.0) workload_var = 0.0;
    } else {
        workload_var = 0.0;
    }
}

double PenaltyState::compute_compactness_for_set(const std::unordered_set<int> &slots) {
    if (slots.empty()) return 0.0;
    std::vector<int> periods;
    periods.reserve(slots.size());
    for (int period : slots) {
        periods.push_back(period);
    }
    std::sort(periods.begin(), periods.end());
    if (periods.empty()) return 0.0;
    double penalty = 0.0;
    for (size_t i = 0; i < periods.size() - 1; ++i) {
        int gap = periods[i + 1] - periods[i];
        if (gap > 1) penalty += gap - 1;
    }
    return penalty;
}

double PenaltyState::compute_compactness_for_teacher_day(const std::string &teacher_id, const std::string &day) const {
    auto it_teacher = day_slots.find(teacher_id);
    if (it_teacher == day_slots.end()) return 0.0;
    auto it_day = it_teacher->second.find(day);
    if (it_day == it_teacher->second.end()) return 0.0;
    return compute_compactness_for_set(it_day->second);
}

void PenaltyState::update_workload(const AssignmentChange &chg, const ProblemData &data) {
    int old_p = ::get_required_periods(data, chg.old_a.course_id, chg.old_a.section_id);
    int new_p = ::get_required_periods(data, chg.new_a.course_id, chg.new_a.section_id);
    
    int w_old_old = workload.count(chg.old_a.teacher_id) ? workload[chg.old_a.teacher_id] : 0;
    int w_old_new = workload.count(chg.new_a.teacher_id) ? workload[chg.new_a.teacher_id] : 0;
    bool same_teacher = (chg.old_a.teacher_id == chg.new_a.teacher_id);
    
    if (w_old_old > 0) {
        sum_workload -= w_old_old;
        sum_workload_squared -= (double)w_old_old * w_old_old;
    }
    if (!same_teacher && w_old_new > 0) {
        sum_workload -= w_old_new;
        sum_workload_squared -= (double)w_old_new * w_old_new;
    }
    
    workload[chg.old_a.teacher_id] -= old_p;
    bool old_teacher_removed = (workload[chg.old_a.teacher_id] <= 0);
    if (old_teacher_removed) workload.erase(chg.old_a.teacher_id);
    workload[chg.new_a.teacher_id] += new_p;
    
    int w_new_old = old_teacher_removed ? 0 : workload[chg.old_a.teacher_id];
    int w_new_new = workload[chg.new_a.teacher_id];
    
    if (same_teacher) {
        if (w_new_new > 0) {
            sum_workload += w_new_new;
            sum_workload_squared += (double)w_new_new * w_new_new;
        }
    } else {
        if (w_new_old > 0) {
            sum_workload += w_new_old;
            sum_workload_squared += (double)w_new_old * w_new_old;
        }
        if (w_new_new > 0) {
            sum_workload += w_new_new;
            sum_workload_squared += (double)w_new_new * w_new_new;
        }
    }
    
    update_workload_var_from_sums();
}

void PenaltyState::update_compactness(const AssignmentChange &chg, const ProblemData &data) {
    double old_compact_old = compute_compactness_for_teacher_day(chg.old_a.teacher_id, chg.old_a.day);
    for_each_slot(chg.old_a, data, [&](const Slot &slot) {
        if (slot.period_idx >= 0 && slot.period_idx < MAX_PERIOD_INDEX) {
            day_slots[chg.old_a.teacher_id][chg.old_a.day].erase(slot.period_idx);
        }
    });
    double new_compact_old = compute_compactness_for_teacher_day(chg.old_a.teacher_id, chg.old_a.day);
    compactness += new_compact_old - old_compact_old;
    
    if (day_slots[chg.old_a.teacher_id][chg.old_a.day].empty()) {
        day_slots[chg.old_a.teacher_id].erase(chg.old_a.day);
    }
    if (day_slots[chg.old_a.teacher_id].empty()) {
        day_slots.erase(chg.old_a.teacher_id);
    }
    
    double old_compact_new = compute_compactness_for_teacher_day(chg.new_a.teacher_id, chg.new_a.day);
    for_each_slot(chg.new_a, data, [&](const Slot &slot) {
        if (slot.period_idx >= 0 && slot.period_idx < MAX_PERIOD_INDEX) {
            day_slots[chg.new_a.teacher_id][chg.new_a.day].insert(slot.period_idx);
        }
    });
    double new_compact_new = compute_compactness_for_teacher_day(chg.new_a.teacher_id, chg.new_a.day);
    compactness += new_compact_new - old_compact_new;
}

double PenaltyState::get_room_penalty() const {
    return room_conflict_penalty + (double)room_capacity_violations * 10.0;
}

void PenaltyState::update_room_penalty_for_assignment(int assignment_idx,
                                                      const OptimalSolution::Assignment &assignment,
                                                      const ProblemData &data) {
    if (assignment.classroom_id.empty()) return;
    
    auto get_room_capacity = [&](const std::string &room_id) -> int {
        for (const auto &room : data.classrooms.classrooms) {
            if (room.id == room_id) return room.capacity;
        }
        return 0;
    };
    
    int day_idx = find_day_index(data.classrooms.days, assignment.day);
    int period_idx = find_period_index(data.classrooms.periods, assignment.period);
    int required_periods = ::get_required_periods(data, assignment.course_id, assignment.section_id);
    
    if (day_idx < 0 || day_idx >= (int)data.classrooms.days.size() ||
        period_idx < 0 || period_idx >= MAX_PERIOD_INDEX || 
        period_idx + required_periods > (int)data.classrooms.periods.size() ||
        period_idx + required_periods > MAX_PERIOD_INDEX) {
        return;
    }
    
    auto &room_day_map = room_slots[assignment.classroom_id];
    auto &room_slot = room_day_map[day_idx];
    
    int conflicts = 0;
    for (int p = 0; p < required_periods; ++p) {
        int period_idx_slot = period_idx + p;
        if (period_idx_slot >= 0 && period_idx_slot < MAX_PERIOD_INDEX) {
            if (room_slot.count(period_idx_slot) > 0) {
                conflicts++;
            }
            room_slot.insert(period_idx_slot);
        }
    }

    room_conflict_penalty += (double)conflicts;
    
    int required_seats = ::get_required_seats(data, assignment.course_id, assignment.section_id);
    int room_cap = get_room_capacity(assignment.classroom_id);
    if (required_seats > room_cap) {
        room_capacity_violations++;
    }
}

void PenaltyState::update_room_conflicts(const AssignmentChange &chg, const ProblemData &data) {
    auto get_room_capacity = [&](const std::string &room_id) -> int {
        for (const auto &room : data.classrooms.classrooms) {
            if (room.id == room_id) return room.capacity;
        }
        return 0;
    };
    
    if (!chg.old_a.classroom_id.empty()) {
        int day_idx = find_day_index(data.classrooms.days, chg.old_a.day);
        int period_idx = find_period_index(data.classrooms.periods, chg.old_a.period);
        int required_periods = ::get_required_periods(data, chg.old_a.course_id, chg.old_a.section_id);
        
        if (day_idx >= 0 && day_idx < (int)data.classrooms.days.size() &&
            period_idx >= 0 && period_idx < MAX_PERIOD_INDEX &&
            period_idx + required_periods <= (int)data.classrooms.periods.size() &&
            period_idx + required_periods <= MAX_PERIOD_INDEX) {
            
            auto it_room = room_slots.find(chg.old_a.classroom_id);
            if (it_room != room_slots.end()) {
                auto it_day = it_room->second.find(day_idx);
                if (it_day != it_room->second.end()) {
                    int old_conflicts = 0;
                    auto &room_slot = it_day->second;
                    for (int p = 0; p < required_periods; ++p) {
                        int period_idx_slot = period_idx + p;
                        if (period_idx_slot >= 0 && period_idx_slot < MAX_PERIOD_INDEX) {
                            if (room_slot.count(period_idx_slot) > 0) {
                                old_conflicts++;
                            }
                            room_slot.erase(period_idx_slot);
                        }
                    }
                    room_conflict_penalty -= (double)old_conflicts;
                    
                    if (room_slot.empty()) {
                        it_room->second.erase(day_idx);
                    }
                }
                
                if (it_room->second.empty()) {
                    room_slots.erase(it_room);
                }
            }
            
            int required_seats = ::get_required_seats(data, chg.old_a.course_id, chg.old_a.section_id);
            int room_cap = get_room_capacity(chg.old_a.classroom_id);
            if (required_seats > room_cap) {
                room_capacity_violations--;
            }
        }
    }
    
    if (!chg.new_a.classroom_id.empty()) {
        int day_idx = find_day_index(data.classrooms.days, chg.new_a.day);
        int period_idx = find_period_index(data.classrooms.periods, chg.new_a.period);
        int required_periods = ::get_required_periods(data, chg.new_a.course_id, chg.new_a.section_id);
        
        if (day_idx >= 0 && day_idx < (int)data.classrooms.days.size() &&
            period_idx >= 0 && period_idx < MAX_PERIOD_INDEX &&
            period_idx + required_periods <= (int)data.classrooms.periods.size() &&
            period_idx + required_periods <= MAX_PERIOD_INDEX) {
            
            auto &room_day_map = room_slots[chg.new_a.classroom_id];
            auto &room_slot = room_day_map[day_idx];
            
            int new_conflicts = 0;
            for (int p = 0; p < required_periods; ++p) {
                int period_idx_slot = period_idx + p;
                if (period_idx_slot >= 0 && period_idx_slot < MAX_PERIOD_INDEX) {
                    if (room_slot.count(period_idx_slot) > 0) {
                        new_conflicts++;
                    }
                    room_slot.insert(period_idx_slot);
                }
            }
            room_conflict_penalty += (double)new_conflicts;
            
            int required_seats = ::get_required_seats(data, chg.new_a.course_id, chg.new_a.section_id);
            int room_cap = get_room_capacity(chg.new_a.classroom_id);
            if (required_seats > room_cap) {
                room_capacity_violations++;
            }
        }
    }
}

void PenaltyState::apply_change(const AssignmentChange &chg, const ProblemData &data) {
    update_workload(chg, data);
    update_compactness(chg, data);
    update_room_conflicts(chg, data);
}

void PenaltyState::revert_change(const AssignmentChange &chg, const ProblemData &data) {
    AssignmentChange reversed_chg = chg;
    std::swap(reversed_chg.old_a, reversed_chg.new_a);
    apply_change(reversed_chg, data);
}

static inline int get_room_capacity(const ProblemData &data, const std::string &room_id) {
    for (const auto &room : data.classrooms.classrooms) {
        if (room.id == room_id) return room.capacity;
    }
    return 0;
}

PenaltyState init_penalty_state(const OptimalSolution &sol, const ProblemData &data) {
    PenaltyState state;
    
    std::unordered_map<std::string, std::unordered_map<int, std::unordered_set<int>>> room_usage;
    
    for (const auto &a : sol.assignments) {
        int required_periods = ::get_required_periods(data, a.course_id, a.section_id);
        state.workload[a.teacher_id] += required_periods;
        
        for_each_slot(a, data, [&](const Slot &slot) {
            if (slot.day_idx >= 0 && slot.day_idx < (int)data.classrooms.days.size() &&
                slot.period_idx >= 0 && slot.period_idx < MAX_PERIOD_INDEX) {
                state.day_slots[a.teacher_id][a.day].insert(slot.period_idx);
                
                if (!a.classroom_id.empty() && slot.day_idx >= 0 && 
                    slot.day_idx < (int)data.classrooms.days.size()) {
                    room_usage[a.classroom_id][slot.day_idx].insert(slot.period_idx);
                }
            }
        });
        
        if (!a.classroom_id.empty()) {
            int required_seats = ::get_required_seats(data, a.course_id, a.section_id);
            int room_cap = get_room_capacity(data, a.classroom_id);
            if (required_seats > room_cap) {
                state.room_capacity_violations++;
            }
        }
    }
    
    std::unordered_map<std::string, std::unordered_map<int, std::unordered_map<int, int>>> period_usage_count;
    for (const auto &a : sol.assignments) {
        if (a.classroom_id.empty()) continue;
        int day_idx = find_day_index(data.classrooms.days, a.day);
        int period_idx = find_period_index(data.classrooms.periods, a.period);
        int required_periods = ::get_required_periods(data, a.course_id, a.section_id);
        
        if (day_idx >= 0 && day_idx < (int)data.classrooms.days.size() &&
            period_idx >= 0 && period_idx < MAX_PERIOD_INDEX &&
            period_idx + required_periods <= (int)data.classrooms.periods.size() &&
            period_idx + required_periods <= MAX_PERIOD_INDEX) {
            for (int p = 0; p < required_periods; ++p) {
                int period_idx_slot = period_idx + p;
                if (period_idx_slot >= 0 && period_idx_slot < MAX_PERIOD_INDEX) {
                    period_usage_count[a.classroom_id][day_idx][period_idx_slot]++;
                }
            }
        }
    }
    
    for (const auto &[room_id, day_map] : period_usage_count) {
        for (const auto &[day_idx, period_map] : day_map) {
            for (const auto &[period_idx, count] : period_map) {
                if (count > 1) {
                    state.room_conflict_penalty += (double)(count - 1);
                }
            }
        }
    }
    
    state.room_slots = room_usage;
    
    for (const auto &[tid, days] : state.day_slots) {
        for (const auto &[day, slots] : days) {
            state.compactness += PenaltyState::compute_compactness_for_set(slots);
        }
    }
    
    state.sum_workload = 0.0;
    state.sum_workload_squared = 0.0;
    for (const auto &tw : state.workload) {
        int w = tw.second;
        state.sum_workload += w;
        state.sum_workload_squared += (double)w * w;
    }
    
    state.update_workload_var_from_sums();
    
    return state;
}

} // namespace phase3

