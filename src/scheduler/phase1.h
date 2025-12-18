#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <map>

using json = nlohmann::json;
using namespace std;

struct TimePref
{
    string day;
    string period;
    int score;
};

struct Teacher
{
    string id;
    string name;
    int max_courses = 1;
    map<string, int> course_pref;
    vector<TimePref> time_pref;
    vector<string> eligible_courses;
    vector<TimePref> LMi;
};

struct Section
{
    string id;
    int required_periods = 1;
    int required_seats = 0; // số lượng chỗ ngồi cần thiết
};

struct Course
{
    string id;
    string name;
    vector<Section> sections;
    int min_teachers = 1;
    int max_teachers = 1;
    vector<string> Ij; // eligible teacher ids sorted by preference
};

struct Classroom
{
    string id;
    int capacity; // sức chứa của lớp học
};

struct ClassroomInfo
{
    vector<string> days;
    vector<string> periods;
    vector<Classroom> classrooms; // danh sách các lớp học với capacity
    map<string, map<string, int>> Clm; // giữ lại để tương thích ngược (deprecated)
};

struct ProblemData
{
    vector<Teacher> teachers;
    vector<Course> courses;
    ClassroomInfo classrooms;
};

ProblemData initialize_problem_from_json(const json &j_input);
