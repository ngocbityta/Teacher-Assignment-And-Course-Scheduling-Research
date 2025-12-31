#include "phase3_index.h"
#include "phase3_invariants.h"
#include "phase_common.h"
#include <algorithm>
#include <sstream>

namespace phase3 {

struct Slot {
    int day_idx;
    int period_idx;
};

static inline int safe_slot_index(int day_idx, int period_idx, int num_periods, const char* context = "") {
    if (day_idx < 0 || period_idx < 0 || num_periods <= 0) return -1;
    if (day_idx >= 1000 || period_idx >= num_periods) return -1;
    int slot_idx = day_idx * num_periods + period_idx;
    if (slot_idx < 0) return -1;
    return slot_idx;
}

// Legacy function for backward compatibility - now uses safe version
static inline int slot_index(int day_idx, int period_idx, int num_periods) {
    return safe_slot_index(day_idx, period_idx, num_periods, "slot_index");
}

template<typename F>
static inline void for_each_slot(const OptimalSolution::Assignment &a, const CachedIndices &cache, F fn) {
    int day_idx = cache.get_day_idx(a.day);
    int period_idx = cache.get_period_idx(a.period);
    int required_periods = cache.get_required_periods(a.course_id, a.section_id);
    
    constexpr int MAX_PERIOD_INDEX = 128;
    if (day_idx < 0 || period_idx < 0) return;
    if (period_idx + required_periods > cache.num_periods) return;
    if (period_idx >= MAX_PERIOD_INDEX || period_idx + required_periods > MAX_PERIOD_INDEX) return;
    
    for (int i = 0; i < required_periods; ++i) {
        int slot_period_idx = period_idx + i;
        if (slot_period_idx >= 0 && slot_period_idx < MAX_PERIOD_INDEX) {
            fn(Slot{day_idx, slot_period_idx});
        }
    }
}

IndexDelta& IndexDelta::operator+=(const IndexDelta& other) {
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

SolIndex build_index(const OptimalSolution &sol, const ProblemData &data) {
    SolIndex idx;
    CachedIndices cache(data);
    idx.num_periods = cache.num_periods;
    
    for (const auto &a : sol.assignments) {
        for_each_slot(a, cache, [&](const Slot &slot) {
            if (slot.day_idx < 0 || slot.period_idx < 0) return;
            if (slot.period_idx >= idx.num_periods || slot.day_idx >= cache.num_days) return;
            
            int slot_idx = safe_slot_index(slot.day_idx, slot.period_idx, idx.num_periods);
            if (slot_idx < 0) return;
            
            idx.teacher_busy_idx[a.teacher_id][slot_idx]++;
            idx.course_slot_section_idx[a.course_id][slot_idx]++;
            idx.course_teachers[a.course_id].insert(a.teacher_id);
            idx.teacher_courses[a.teacher_id].insert(a.course_id);
            idx.course_teacher_sections[a.course_id + "|" + a.teacher_id].insert(a.section_id);
            if (!a.classroom_id.empty()) {
                idx.classroom_busy_idx[a.classroom_id][slot_idx]++;
            }
        });
    }
    
    return idx;
}

void apply_index_delta(SolIndex &idx, const IndexDelta &d) {
    auto erase_if_empty = [](auto &map, const auto &key) {
        if (map[key].empty()) map.erase(key);
    };
    
    for (const auto &[teacher_id, slot_idx] : d.teacher_remove_idx) {
        if (slot_idx >= 0) {
            auto &slot_map = idx.teacher_busy_idx[teacher_id];
            slot_map[slot_idx]--;
            if (slot_map[slot_idx] <= 0) slot_map.erase(slot_idx);
            erase_if_empty(idx.teacher_busy_idx, teacher_id);
        }
    }
    for (const auto &[teacher_id, slot_idx] : d.teacher_add_idx) {
        if (slot_idx >= 0) {
            idx.teacher_busy_idx[teacher_id][slot_idx]++;
        }
    }
    for (const auto &[course_id, slot_idx, section_id] : d.course_slot_section_remove_idx) {
        if (slot_idx >= 0) {
            auto &slot_map = idx.course_slot_section_idx[course_id];
            slot_map[slot_idx]--;
            if (slot_map[slot_idx] <= 0) slot_map.erase(slot_idx);
            erase_if_empty(idx.course_slot_section_idx, course_id);
        }
    }
    for (const auto &[course_id, slot_idx, section_id] : d.course_slot_section_add_idx) {
        if (slot_idx >= 0) {
            idx.course_slot_section_idx[course_id][slot_idx]++;
        }
    }
    for (const auto &[course_id, teacher_id, section_id] : d.course_teacher_section_remove) {
        std::string ctk = course_id + "|" + teacher_id;
        idx.course_teacher_sections[ctk].erase(section_id);
        if (idx.course_teacher_sections[ctk].empty()) {
            idx.course_teacher_sections.erase(ctk);
            idx.course_teachers[course_id].erase(teacher_id);
            erase_if_empty(idx.course_teachers, course_id);
            idx.teacher_courses[teacher_id].erase(course_id);
            erase_if_empty(idx.teacher_courses, teacher_id);
        }
    }
    for (const auto &[course_id, teacher_id, section_id] : d.course_teacher_section_add) {
        std::string ctk = course_id + "|" + teacher_id;
        if (idx.course_teacher_sections[ctk].empty()) {
            idx.course_teachers[course_id].insert(teacher_id);
            idx.teacher_courses[teacher_id].insert(course_id);
        }
        idx.course_teacher_sections[ctk].insert(section_id);
    }
    for (const auto &[classroom_id, slot_idx] : d.classroom_remove_idx) {
        if (slot_idx >= 0) {
            auto &slot_map = idx.classroom_busy_idx[classroom_id];
            slot_map[slot_idx]--;
            if (slot_map[slot_idx] <= 0) slot_map.erase(slot_idx);
            erase_if_empty(idx.classroom_busy_idx, classroom_id);
        }
    }
    for (const auto &[classroom_id, slot_idx] : d.classroom_add_idx) {
        if (slot_idx >= 0) {
            idx.classroom_busy_idx[classroom_id][slot_idx]++;
        }
    }
}

IndexDelta build_delta(
    const OptimalSolution::Assignment &old_a,
    const OptimalSolution::Assignment &new_a,
    DeltaMode mode,
    const ProblemData &data) {
    IndexDelta delta;
    CachedIndices cache(data);
    int num_periods = cache.num_periods;
    
    auto process_slot = [&](const OptimalSolution::Assignment &a, bool is_add) {
        for_each_slot(a, cache, [&](const Slot &slot) {
            if (slot.day_idx < 0 || slot.period_idx < 0 || slot.period_idx >= num_periods) return;
            
            int slot_idx = safe_slot_index(slot.day_idx, slot.period_idx, num_periods);
            if (slot_idx < 0) return;
            
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

} // namespace phase3

