#include "phase3_invariants.h"
#include <unordered_map>
#include <unordered_set>
#include <iostream>

namespace phase3 {

static inline int slot_index(int day_idx, int period_idx, int num_periods) {
    // Safety check: prevent integer overflow and invalid indices
    if (day_idx < 0 || period_idx < 0 || num_periods <= 0) return -1;
    if (day_idx >= 10000 || period_idx >= num_periods) return -1; // Reasonable upper bound
    
    int result = day_idx * num_periods + period_idx;
    // Check for overflow
    if (result < 0) return -1;
    return result;
}

// Implementation of CachedIndices
CachedIndices::CachedIndices(const ProblemData &data) {
    num_days = (int)data.classrooms.days.size();
    num_periods = (int)data.classrooms.periods.size();
    total_slots = num_days * num_periods;
    
    // Safety check: only cache if vectors are not empty
    if (num_days > 0) {
        // Cache day indices
        for (int d = 0; d < num_days; ++d) {
            if (d < (int)data.classrooms.days.size()) {
                day_to_idx[data.classrooms.days[d]] = d;
            }
        }
    }
    
    if (num_periods > 0) {
        // Cache period indices
        for (int p = 0; p < num_periods; ++p) {
            if (p < (int)data.classrooms.periods.size()) {
                period_to_idx[data.classrooms.periods[p]] = p;
            }
        }
    }
    
    // Cache required_periods
    for (const auto &c : data.courses) {
        for (const auto &s : c.sections) {
            std::string key = c.id + "|" + s.id;
            course_section_to_periods[key] = s.required_periods;
        }
    }
}

int CachedIndices::get_day_idx(const std::string &day) const {
    auto it = day_to_idx.find(day);
    return (it != day_to_idx.end()) ? it->second : -1;
}

int CachedIndices::get_period_idx(const std::string &period) const {
    auto it = period_to_idx.find(period);
    return (it != period_to_idx.end()) ? it->second : -1;
}

int CachedIndices::get_required_periods(const std::string &course_id, const std::string &section_id) const {
    std::string key = course_id + "|" + section_id;
    auto it = course_section_to_periods.find(key);
    return (it != course_section_to_periods.end()) ? it->second : 1;
}

// Implementation of CachedLookups
CachedLookups::CachedLookups(const ProblemData &data) {
    // Cache classroom capacity
    for (const auto &room : data.classrooms.classrooms) {
        classroom_capacity[room.id] = room.capacity;
    }
    
    // Cache teacher eligible courses
    for (const auto &teacher : data.teachers) {
        std::unordered_set<std::string> eligible_set;
        for (const auto &course_id : teacher.eligible_courses) {
            eligible_set.insert(course_id);
        }
        teacher_eligible_courses[teacher.id] = std::move(eligible_set);
    }
    
    // Cache course section required seats
    for (const auto &course : data.courses) {
        for (const auto &section : course.sections) {
            std::string key = course.id + "|" + section.id;
            course_section_seats[key] = section.required_seats;
        }
    }
}

bool CachedLookups::classroom_exists(const std::string &classroom_id) const {
    return classroom_capacity.find(classroom_id) != classroom_capacity.end();
}

int CachedLookups::get_classroom_capacity(const std::string &classroom_id) const {
    auto it = classroom_capacity.find(classroom_id);
    return (it != classroom_capacity.end()) ? it->second : 0;
}

bool CachedLookups::is_teacher_eligible(const std::string &teacher_id, const std::string &course_id) const {
    auto it = teacher_eligible_courses.find(teacher_id);
    if (it == teacher_eligible_courses.end()) {
        return false;
    }
    return it->second.find(course_id) != it->second.end();
}

int CachedLookups::get_required_seats(const std::string &course_id, const std::string &section_id) const {
    std::string key = course_id + "|" + section_id;
    auto it = course_section_seats.find(key);
    return (it != course_section_seats.end()) ? it->second : 0;
}

struct Slot {
    int day_idx;
    int period_idx;
};

template<typename F>
static inline void for_each_slot(const OptimalSolution::Assignment &a, const CachedIndices &cache, F fn) {
    int day_idx = cache.get_day_idx(a.day);
    int period_idx = cache.get_period_idx(a.period);
    int required_periods = cache.get_required_periods(a.course_id, a.section_id);
    if (day_idx < 0 || day_idx >= cache.num_days) return;
    if (period_idx < 0 || period_idx >= cache.num_periods) return;
    if (period_idx + required_periods > cache.num_periods) return;
    
    for (int i = 0; i < required_periods; ++i) {
        int current_period_idx = period_idx + i;
        if (current_period_idx >= 0 && current_period_idx < cache.num_periods) {
            fn(Slot{day_idx, current_period_idx});
        }
    }
}

bool is_teacher_eligible(const ProblemData &data, const std::string &teacher_id, const std::string &course_id) {
    // Use CachedLookups for better performance
    CachedLookups lookups(data);
    return lookups.is_teacher_eligible(teacher_id, course_id);
}

std::string check_assignment_hard_invariant(
    const OptimalSolution::Assignment &assignment,
    const ProblemData &data,
    int assignment_index) {
    
    CachedIndices cache(data);
    CachedLookups lookups(data);
    
    // Check 1: Missing classroom_id (HARD CONSTRAINT)
    if (assignment.classroom_id.empty()) {
        return "Assignment " + std::to_string(assignment_index) + ": Missing classroom_id";
    }
    
    // Check 2: Teacher eligibility (HARD CONSTRAINT)
    if (!lookups.is_teacher_eligible(assignment.teacher_id, assignment.course_id)) {
        return "Assignment " + std::to_string(assignment_index) + 
               ": Teacher '" + assignment.teacher_id + 
               "' is not eligible for course '" + assignment.course_id + "'";
    }
    
    // Check 3: Valid day and period indices
    int day_idx = cache.get_day_idx(assignment.day);
    int period_idx = cache.get_period_idx(assignment.period);
    int required_periods = cache.get_required_periods(assignment.course_id, assignment.section_id);
    
    if (day_idx < 0) {
        return "Assignment " + std::to_string(assignment_index) + ": Invalid day '" + assignment.day + "'";
    }
    if (period_idx < 0) {
        return "Assignment " + std::to_string(assignment_index) + ": Invalid period '" + assignment.period + "'";
    }
    if (period_idx + required_periods > cache.num_periods) {
        return "Assignment " + std::to_string(assignment_index) + 
               ": Period range exceeds available periods";
    }
    
    // Check 4: Period continuity (HARD CONSTRAINT)
    // For multi-period lectures, periods must be consecutive
    // This is already enforced by the period_idx + required_periods check above
    // and the fact that we iterate through consecutive periods
    
    // Check 5: Classroom exists
    if (!lookups.classroom_exists(assignment.classroom_id)) {
        return "Assignment " + std::to_string(assignment_index) + 
               ": Classroom '" + assignment.classroom_id + "' does not exist";
    }
    
    // Check 6: Classroom capacity
    int required_seats = lookups.get_required_seats(assignment.course_id, assignment.section_id);
    int classroom_capacity = lookups.get_classroom_capacity(assignment.classroom_id);
    if (required_seats > classroom_capacity) {
        return "Assignment " + std::to_string(assignment_index) + 
               ": Classroom '" + assignment.classroom_id + 
               "' capacity (" + std::to_string(classroom_capacity) + 
               ") < required seats (" + std::to_string(required_seats) + ")";
    }
    
    return ""; // All checks passed
}

bool check_hard_invariant(const OptimalSolution &solution, const ProblemData &data) {
    CachedIndices cache(data);
    CachedLookups lookups(data);
    
    std::unordered_map<std::string, std::unordered_set<int>> teacher_slots;
    std::unordered_map<std::string, std::unordered_set<int>> classroom_slots;
    std::unordered_map<std::string, std::unordered_map<int, std::string>> course_slot_section;
    
    std::unordered_map<std::string, std::unordered_set<std::string>> teacher_courses;
    std::unordered_map<std::string, std::unordered_set<std::string>> course_teachers;
    
    for (size_t i = 0; i < solution.assignments.size(); ++i) {
        const auto &a = solution.assignments[i];
        
        // Check 1: Missing classroom_id
        if (a.classroom_id.empty()) {
            return false;
        }
        
        // Check 2: Teacher eligibility
        if (!lookups.is_teacher_eligible(a.teacher_id, a.course_id)) {
            return false;
        }
        
        // Check 3: Valid day and period indices
        int day_idx = cache.get_day_idx(a.day);
        int period_idx = cache.get_period_idx(a.period);
        int required_periods = cache.get_required_periods(a.course_id, a.section_id);
        
        if (day_idx < 0 || period_idx < 0 || period_idx + required_periods > cache.num_periods) {
            return false;
        }
        
        // Check 4: Classroom exists
        if (!lookups.classroom_exists(a.classroom_id)) {
            return false;
        }
        
        // Check 5: Classroom capacity
        int required_seats = lookups.get_required_seats(a.course_id, a.section_id);
        int classroom_capacity = lookups.get_classroom_capacity(a.classroom_id);
        if (required_seats > classroom_capacity) {
            return false;
        }
        
        teacher_courses[a.teacher_id].insert(a.course_id);
        course_teachers[a.course_id].insert(a.teacher_id);
        
        // Check 6-8: Time conflicts (teacher, classroom, course/section)
        bool has_conflict = false;
        for_each_slot(a, cache, [&](const Slot &slot) {
            if (has_conflict) return;
            int slot_idx = slot_index(slot.day_idx, slot.period_idx, cache.num_periods);
            
            // Safety check: skip invalid slot indices
            if (slot_idx < 0) return;
            
            // Teacher time conflict
            if (teacher_slots[a.teacher_id].count(slot_idx)) {
                has_conflict = true;
                return;
            }
            
            // Classroom time conflict
            if (classroom_slots[a.classroom_id].count(slot_idx)) {
                has_conflict = true;
                return;
            }
            
            // Course/section time conflict
            auto it = course_slot_section.find(a.course_id);
            if (it != course_slot_section.end()) {
                auto it_slot = it->second.find(slot_idx);
                if (it_slot != it->second.end() && it_slot->second != a.section_id) {
                    has_conflict = true;
                    return;
                }
            }
            
            teacher_slots[a.teacher_id].insert(slot_idx);
            classroom_slots[a.classroom_id].insert(slot_idx);
            course_slot_section[a.course_id][slot_idx] = a.section_id;
        });
        
        if (has_conflict) {
            return false;
        }
    }
    
    // Check 9: Teacher max_courses constraint
    for (const auto &teacher : data.teachers) {
        auto it = teacher_courses.find(teacher.id);
        if (it != teacher_courses.end()) {
            int courses_count = (int)it->second.size();
            if (courses_count > teacher.max_courses) {
                return false;
            }
        }
    }
    
    // Check 10: Course min/max teachers constraint
    for (const auto &course : data.courses) {
        auto it = course_teachers.find(course.id);
        int num_teachers = (it != course_teachers.end()) ? (int)it->second.size() : 0;
        if (course.min_teachers > 0 && num_teachers < course.min_teachers) {
            return false;
        }
        if (course.max_teachers > 0 && num_teachers > course.max_teachers) {
            return false;
        }
    }
    
    return true;
}

int count_hard_violations(const OptimalSolution &solution, const ProblemData &data) {
    int violations = 0;
    CachedIndices cache(data);
    CachedLookups lookups(data);
    
    // Use counting maps for time conflicts
    std::unordered_map<std::string, std::unordered_map<int, int>> teacher_slot_count;
    std::unordered_map<std::string, std::unordered_map<int, int>> classroom_slot_count;
    std::unordered_map<std::string, std::unordered_map<int, std::unordered_map<std::string, int>>> course_slot_section_count;
    
    std::unordered_map<std::string, std::unordered_set<std::string>> teacher_courses;
    std::unordered_map<std::string, std::unordered_set<std::string>> course_teachers;
    
    for (const auto &a : solution.assignments) {
        // Check 1: Missing classroom_id
        if (a.classroom_id.empty()) {
            violations++;
        }
        
        // Check 2: Teacher eligibility
        if (!lookups.is_teacher_eligible(a.teacher_id, a.course_id)) {
            violations++;
        }
        
        teacher_courses[a.teacher_id].insert(a.course_id);
        course_teachers[a.course_id].insert(a.teacher_id);
        
        // Check 3: Valid day and period indices
        int day_idx = cache.get_day_idx(a.day);
        int period_idx = cache.get_period_idx(a.period);
        int required_periods = cache.get_required_periods(a.course_id, a.section_id);
        
        if (day_idx < 0 || period_idx < 0 || period_idx + required_periods > cache.num_periods) {
            violations++;
            continue; // Skip time conflict checks if time is invalid
        }
        
        // Check 4: Classroom exists
        if (!lookups.classroom_exists(a.classroom_id)) {
            violations++;
        }
        
        // Check 5: Classroom capacity
        if (!a.classroom_id.empty() && lookups.classroom_exists(a.classroom_id)) {
            int required_seats = lookups.get_required_seats(a.course_id, a.section_id);
            int classroom_capacity = lookups.get_classroom_capacity(a.classroom_id);
            if (required_seats > classroom_capacity) {
                violations++;
            }
        }
        
        // Track time conflicts (will count later)
        for_each_slot(a, cache, [&](const Slot &slot) {
            int slot_idx = slot_index(slot.day_idx, slot.period_idx, cache.num_periods);
            
            if (slot_idx < 0) return;
            
            teacher_slot_count[a.teacher_id][slot_idx]++;
            
            if (!a.classroom_id.empty()) {
                classroom_slot_count[a.classroom_id][slot_idx]++;
            }
            
            course_slot_section_count[a.course_id][slot_idx][a.section_id]++;
        });
    }
    
    // Count time conflict violations
    // Check 6: Teacher time conflicts
    for (const auto &[teacher_id, slot_map] : teacher_slot_count) {
        for (const auto &[slot_idx, count] : slot_map) {
            if (count > 1) {
                violations += (count - 1);
            }
        }
    }
    
    // Check 7: Classroom time conflicts
    for (const auto &[classroom_id, slot_map] : classroom_slot_count) {
        for (const auto &[slot_idx, count] : slot_map) {
            if (count > 1) {
                violations += (count - 1);
            }
        }
    }
    
    // Check 8: Course/section time conflicts (same course, different section at same time)
    for (const auto &[course_id, slot_section_map] : course_slot_section_count) {
        for (const auto &[slot_idx, section_map] : slot_section_map) {
            int total_sections = 0;
            for (const auto &[section_id, count] : section_map) {
                total_sections += count;
            }
            if (total_sections > 1) {
                violations += (total_sections - 1);
            }
        }
    }
    
    // Check 9: Teacher max_courses constraint
    for (const auto &teacher : data.teachers) {
        auto it = teacher_courses.find(teacher.id);
        if (it != teacher_courses.end()) {
            int courses_count = (int)it->second.size();
            if (courses_count > teacher.max_courses) {
                violations += (courses_count - teacher.max_courses);
            }
        }
    }
    
    // Check 10: Course min/max teachers constraint
    for (const auto &course : data.courses) {
        auto it = course_teachers.find(course.id);
        int num_teachers = (it != course_teachers.end()) ? (int)it->second.size() : 0;
        if (course.min_teachers > 0 && num_teachers < course.min_teachers) {
            violations += (course.min_teachers - num_teachers);
        }
        if (course.max_teachers > 0 && num_teachers > course.max_teachers) {
            violations += (num_teachers - course.max_teachers);
        }
    }
    
    return violations;
}

void report_constraint_violations(const OptimalSolution &solution, const ProblemData &data) {
    CachedIndices cache(data);
    CachedLookups lookups(data);
    
    int missing_classroom_count = 0;
    int teacher_eligibility_count = 0;
    int invalid_time_count = 0;
    int classroom_not_exist_count = 0;
    int classroom_capacity_count = 0;
    int teacher_time_conflict_count = 0;
    int classroom_time_conflict_count = 0;
    int course_section_conflict_count = 0;
    int teacher_max_courses_count = 0;
    int course_min_teachers_count = 0;
    int course_max_teachers_count = 0;
    
    std::unordered_map<std::string, std::unordered_map<int, int>> teacher_slot_assignments;
    std::unordered_map<std::string, std::unordered_map<int, int>> classroom_slot_assignments;
    std::unordered_map<std::string, std::unordered_set<int>> teacher_slots;
    std::unordered_map<std::string, std::unordered_set<int>> classroom_slots;
    std::unordered_map<std::string, std::unordered_map<int, std::string>> course_slot_section;
    
    std::unordered_map<std::string, std::unordered_set<std::string>> teacher_courses;
    std::unordered_map<std::string, std::unordered_set<std::string>> course_teachers;
    
    for (size_t i = 0; i < solution.assignments.size(); ++i) {
        const auto &a = solution.assignments[i];
        
        if (a.classroom_id.empty()) {
            missing_classroom_count++;
            std::cerr << "  [CONSTRAINT 1] Assignment " << i << " (teacher=" << a.teacher_id 
                      << ", course=" << a.course_id << ", section=" << a.section_id 
                      << "): Missing classroom_id" << std::endl;
            continue;
        }
        
        if (!lookups.is_teacher_eligible(a.teacher_id, a.course_id)) {
            teacher_eligibility_count++;
            std::cerr << "  [CONSTRAINT 2] Assignment " << i << ": Teacher '" << a.teacher_id 
                      << "' is not eligible for course '" << a.course_id << "'" << std::endl;
        }
        
        int day_idx = cache.get_day_idx(a.day);
        int period_idx = cache.get_period_idx(a.period);
        int required_periods = cache.get_required_periods(a.course_id, a.section_id);
        
        if (day_idx < 0 || period_idx < 0 || period_idx + required_periods > cache.num_periods) {
            invalid_time_count++;
            std::cerr << "  [CONSTRAINT 3] Assignment " << i << ": Invalid time (day='" << a.day 
                      << "', period='" << a.period << "', required_periods=" << required_periods 
                      << ", max_periods=" << cache.num_periods << ")" << std::endl;
            continue;
        }
        
        if (!lookups.classroom_exists(a.classroom_id)) {
            classroom_not_exist_count++;
            std::cerr << "  [CONSTRAINT 4] Assignment " << i << ": Classroom '" << a.classroom_id 
                      << "' does not exist" << std::endl;
            continue;
        }
        
        int required_seats = lookups.get_required_seats(a.course_id, a.section_id);
        int classroom_capacity = lookups.get_classroom_capacity(a.classroom_id);
        if (required_seats > classroom_capacity) {
            classroom_capacity_count++;
            std::cerr << "  [CONSTRAINT 5] Assignment " << i << ": Classroom '" << a.classroom_id 
                      << "' capacity (" << classroom_capacity << ") < required seats (" 
                      << required_seats << ") for course=" << a.course_id << ", section=" << a.section_id << std::endl;
        }
        
        teacher_courses[a.teacher_id].insert(a.course_id);
        course_teachers[a.course_id].insert(a.teacher_id);
        
        for_each_slot(a, cache, [&](const Slot &slot) {
            // Safety check: ensure vectors are not empty and indices are within bounds
            if (data.classrooms.days.empty() || data.classrooms.periods.empty()) return;
            if (slot.day_idx < 0 || slot.day_idx >= (int)data.classrooms.days.size()) return;
            if (slot.period_idx < 0 || slot.period_idx >= (int)data.classrooms.periods.size()) return;
            
            int slot_idx = slot_index(slot.day_idx, slot.period_idx, cache.num_periods);
            
            if (slot_idx < 0) return;
            
            if (teacher_slots[a.teacher_id].count(slot_idx)) {
                teacher_time_conflict_count++;
                auto it_assign = teacher_slot_assignments[a.teacher_id].find(slot_idx);
                int conflict_idx = (it_assign != teacher_slot_assignments[a.teacher_id].end()) ? it_assign->second : -1;
                std::cerr << "  [CONSTRAINT 6] Assignment " << i << " conflicts with assignment " << conflict_idx 
                          << ": Teacher '" << a.teacher_id << "' has two classes at same time (day=" 
                          << data.classrooms.days[slot.day_idx] << ", period=" 
                          << data.classrooms.periods[slot.period_idx] << ")" << std::endl;
            } else {
                teacher_slots[a.teacher_id].insert(slot_idx);
                teacher_slot_assignments[a.teacher_id][slot_idx] = (int)i;
            }
            
            if (classroom_slots[a.classroom_id].count(slot_idx)) {
                classroom_time_conflict_count++;
                auto it_assign = classroom_slot_assignments[a.classroom_id].find(slot_idx);
                int conflict_idx = (it_assign != classroom_slot_assignments[a.classroom_id].end()) ? it_assign->second : -1;
                std::cerr << "  [CONSTRAINT 7] Assignment " << i << " conflicts with assignment " << conflict_idx 
                          << ": Classroom '" << a.classroom_id << "' is double-booked at (day=" 
                          << data.classrooms.days[slot.day_idx] << ", period=" 
                          << data.classrooms.periods[slot.period_idx] << ")" << std::endl;
            } else {
                classroom_slots[a.classroom_id].insert(slot_idx);
                classroom_slot_assignments[a.classroom_id][slot_idx] = (int)i;
            }
            
            auto it = course_slot_section.find(a.course_id);
            if (it != course_slot_section.end()) {
                auto it_slot = it->second.find(slot_idx);
                if (it_slot != it->second.end() && it_slot->second != a.section_id) {
                    course_section_conflict_count++;
                    std::cerr << "  [CONSTRAINT 8] Assignment " << i << ": Course '" << a.course_id 
                              << "' has multiple sections at same time (section " << a.section_id 
                              << " conflicts with section " << it_slot->second << " at day=" 
                              << data.classrooms.days[slot.day_idx] << ", period=" 
                              << data.classrooms.periods[slot.period_idx] << ")" << std::endl;
                }
            }
            course_slot_section[a.course_id][slot_idx] = a.section_id;
        });
    }
    
    // Constraint 9: Teacher max_courses
    for (const auto &teacher : data.teachers) {
        auto it = teacher_courses.find(teacher.id);
        if (it != teacher_courses.end()) {
            int courses_count = (int)it->second.size();
            if (courses_count > teacher.max_courses) {
                teacher_max_courses_count++;
                std::cerr << "  [CONSTRAINT 9] Teacher '" << teacher.id << "' teaches " << courses_count 
                          << " courses, exceeds max_courses=" << teacher.max_courses << std::endl;
            }
        }
    }
    
    // Constraint 10: Course min/max teachers
    for (const auto &course : data.courses) {
        auto it = course_teachers.find(course.id);
        int num_teachers = (it != course_teachers.end()) ? (int)it->second.size() : 0;
        if (course.min_teachers > 0 && num_teachers < course.min_teachers) {
            course_min_teachers_count++;
            std::cerr << "  [CONSTRAINT 10a] Course '" << course.id << "' has " << num_teachers 
                      << " teachers, below min_teachers=" << course.min_teachers << std::endl;
        }
        if (course.max_teachers > 0 && num_teachers > course.max_teachers) {
            course_max_teachers_count++;
            std::cerr << "  [CONSTRAINT 10b] Course '" << course.id << "' has " << num_teachers 
                      << " teachers, exceeds max_teachers=" << course.max_teachers << std::endl;
        }
    }
    
    // Summary
    std::cerr << std::endl << "=== CONSTRAINT VIOLATION SUMMARY ===" << std::endl;
    std::cerr << "  [1] Missing classroom_id: " << missing_classroom_count << std::endl;
    std::cerr << "  [2] Teacher eligibility: " << teacher_eligibility_count << std::endl;
    std::cerr << "  [3] Invalid time: " << invalid_time_count << std::endl;
    std::cerr << "  [4] Classroom not exist: " << classroom_not_exist_count << std::endl;
    std::cerr << "  [5] Classroom capacity: " << classroom_capacity_count << std::endl;
    std::cerr << "  [6] Teacher-time conflict: " << teacher_time_conflict_count << std::endl;
    std::cerr << "  [7] Classroom-time conflict: " << classroom_time_conflict_count << std::endl;
    std::cerr << "  [8] Course/section-time conflict: " << course_section_conflict_count << std::endl;
    std::cerr << "  [9] Teacher max_courses: " << teacher_max_courses_count << std::endl;
    std::cerr << "  [10a] Course min_teachers: " << course_min_teachers_count << std::endl;
    std::cerr << "  [10b] Course max_teachers: " << course_max_teachers_count << std::endl;
    std::cerr << "=== TOTAL VIOLATIONS: " << (missing_classroom_count + teacher_eligibility_count + 
              invalid_time_count + classroom_not_exist_count + classroom_capacity_count + 
              teacher_time_conflict_count + classroom_time_conflict_count + course_section_conflict_count + 
              teacher_max_courses_count + course_min_teachers_count + course_max_teachers_count) << " ===" << std::endl;
}

} // namespace phase3

