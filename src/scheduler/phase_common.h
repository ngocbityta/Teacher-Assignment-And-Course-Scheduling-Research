#pragma once

#include "phase1.h"
#include <string>
#include <vector>
#include <sstream>
#include <iostream>
#include <algorithm>

// ============================================================================
// Common Utility Functions for Phase 1, 2, 3
// ============================================================================

// Find day index in days vector
inline int find_day_index(const std::vector<std::string> &days, const std::string &day) {
    for (int d = 0; d < (int)days.size(); ++d) {
        if (days[d] == day) return d;
    }
    return -1;
}

// Find period index in periods vector
inline int find_period_index(const std::vector<std::string> &periods, const std::string &period) {
    for (int m = 0; m < (int)periods.size(); ++m) {
        if (periods[m] == period) return m;
    }
    return -1;
}

// Calculate slot index from day and period indices
inline int slot_index(int day_idx, int period_idx, int num_periods) {
    return day_idx * num_periods + period_idx;
}

// Get required periods for a course section
inline int get_required_periods(const ProblemData &data, const std::string &course_id, const std::string &section_id) {
    for (const auto &c : data.courses) {
        if (c.id == course_id) {
            for (const auto &s : c.sections) {
                if (s.id == section_id) {
                    return s.required_periods;
                }
            }
        }
    }
    return 1; // Default to 1 if not found
}

// Get required seats for a course section
inline int get_required_seats(const ProblemData &data, const std::string &course_id, const std::string &section_id) {
    for (const auto &c : data.courses) {
        if (c.id == course_id) {
            for (const auto &s : c.sections) {
                if (s.id == section_id) {
                    return s.required_seats;
                }
            }
        }
    }
    return 0; // Default to 0 if not found
}

// Format timeslot key from day and period
inline std::string format_timeslot_key(const std::string &day, const std::string &period) {
    return day + "|" + period;
}

// Check if teacher is eligible for course
inline bool is_teacher_eligible_for_course(const Teacher &teacher, const std::string &course_id) {
    return std::find(teacher.eligible_courses.begin(), teacher.eligible_courses.end(), 
                     course_id) != teacher.eligible_courses.end();
}

// ============================================================================
// Logging Utilities
// ============================================================================

enum class LogLevel {
    INFO,
    WARNING,
    ERROR,
    DEBUG
};

class PhaseLogger {
private:
    static bool verbose_mode;
    static std::string current_phase;
    
public:
    static void set_verbose(bool enabled) { verbose_mode = enabled; }
    static void set_phase(const std::string &phase) { current_phase = phase; }
    
    static void log(LogLevel level, const std::string &message) {
        if (!verbose_mode && level == LogLevel::DEBUG) return;
        
        std::string prefix = "[" + current_phase + "]";
        switch (level) {
            case LogLevel::INFO:
                std::cout << prefix << " [INFO] " << message << std::endl;
                break;
            case LogLevel::WARNING:
                std::cerr << prefix << " [WARN] " << message << std::endl;
                break;
            case LogLevel::ERROR:
                std::cerr << prefix << " [ERROR] " << message << std::endl;
                break;
            case LogLevel::DEBUG:
                std::cerr << prefix << " [DEBUG] " << message << std::endl;
                break;
        }
    }
    
    static void info(const std::string &message) { log(LogLevel::INFO, message); }
    static void warn(const std::string &message) { log(LogLevel::WARNING, message); }
    static void error(const std::string &message) { log(LogLevel::ERROR, message); }
    static void debug(const std::string &message) { log(LogLevel::DEBUG, message); }
};

// Note: Static members should be initialized in phase_common.cpp

