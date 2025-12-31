#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <map>

using json = nlohmann::json;

struct TimePref
{
    std::string day;
    std::string period;
    int score;
};

struct Teacher
{
    std::string id;
    std::string name;
    int max_courses = 1;
    int max_periods = 0; // 0 means no limit (HARD CONSTRAINT)
    std::map<std::string, int> course_pref;
    std::vector<TimePref> time_pref;
    std::vector<std::string> eligible_courses;
    std::vector<TimePref> LMi;
};

struct Section
{
    std::string id;
    int required_periods = 1;
    int required_seats = 0; // số lượng chỗ ngồi cần thiết
};

struct Course
{
    std::string id;
    std::string name;
    std::vector<Section> sections;
    int min_teachers = 1;
    int max_teachers = 1;
    std::vector<std::string> Ij; // eligible teacher ids sorted by preference
};

struct Classroom
{
    std::string id;
    int capacity; // sức chứa của lớp học
};

struct ClassroomInfo
{
    std::vector<std::string> days;
    std::vector<std::string> periods;
    std::vector<Classroom> classrooms; // danh sách các lớp học với capacity
};

struct ProblemData
{
    std::vector<Teacher> teachers;
    std::vector<Course> courses;
    ClassroomInfo classrooms;
};

ProblemData initialize_problem_from_json(const json &j_input);
