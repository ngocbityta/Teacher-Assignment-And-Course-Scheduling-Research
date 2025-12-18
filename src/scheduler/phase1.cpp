#include "phase1.h"
#include <algorithm>
#include <iostream>

using namespace std;

static void fill_teacher_from_json(Teacher &teacher, const json &jt)
{
    teacher.id = jt.value("id", string());
    teacher.name = jt.value("name", string());
    teacher.max_courses = jt.value("max_courses", 1);
    if (jt.contains("eligible_courses"))
        teacher.eligible_courses = jt["eligible_courses"].get<vector<string>>();
    if (jt.contains("course_preferences"))
        teacher.course_pref = jt["course_preferences"].get<map<string, int>>();

    if (jt.contains("day_time_preferences"))
    {
        for (auto it = jt["day_time_preferences"].begin(); it != jt["day_time_preferences"].end(); ++it)
        {
            string day = it.key();
            const json &periods = it.value();
            for (auto pit = periods.begin(); pit != periods.end(); ++pit)
            {
                string period = pit.key();
                int score = pit.value().get<int>();
                teacher.time_pref.push_back({day, period, score});
            }
        }
    }

    sort(teacher.time_pref.begin(), teacher.time_pref.end(),
         [](const TimePref &a, const TimePref &b)
         { return a.score > b.score; });

    teacher.LMi = teacher.time_pref;
}

static void fill_course_from_json(Course &course, const json &jc)
{
    course.id = jc.value("id", string());
    course.name = jc.value("name", string());
    course.min_teachers = jc.value("min_teachers", 1);
    course.max_teachers = jc.value("max_teachers", 1);

    if (jc.contains("sections"))
    {
        for (const auto &s : jc["sections"])
        {
            Section sec;
            sec.id = s.value("id", string());
            sec.required_periods = s.value("required_periods", 1);
            sec.required_seats = s.value("required_seats", 0);
            course.sections.push_back(sec);
        }
    }
}

ProblemData initialize_problem_from_json(const json &j_input)
{
    ProblemData data;

    if (!j_input.contains("teachers") || !j_input.contains("courses") || !j_input.contains("classrooms"))
    {
        throw runtime_error("Input JSON must contain keys: teachers, courses, classrooms");
    }

    // Teachers
    for (const auto &jt : j_input["teachers"])
    {
        Teacher t;
        fill_teacher_from_json(t, jt);
        data.teachers.push_back(std::move(t));
    }

    // Courses
    for (const auto &jc : j_input["courses"])
    {
        Course c;
        fill_course_from_json(c, jc);
        data.courses.push_back(std::move(c));
    }

    // Classrooms
    const json &jc = j_input["classrooms"];
    data.classrooms.days = jc.value("days", vector<string>{});
    data.classrooms.periods = jc.value("periods", vector<string>{});
    
    // Đọc danh sách classrooms với capacity
    if (jc.contains("classrooms"))
    {
        for (const auto &cl : jc["classrooms"])
        {
            Classroom room;
            room.id = cl.value("id", string());
            room.capacity = cl.value("capacity", 0);
            data.classrooms.classrooms.push_back(room);
        }
    }
    
    // Giữ lại để tương thích ngược (deprecated)
    if (jc.contains("classrooms_per_slot"))
        data.classrooms.Clm = jc["classrooms_per_slot"].get<map<string, map<string, int>>>();

    // Construct Ij for each course: list of teacher ids eligible for this course, sorted by teacher's course preference
    for (auto &course : data.courses)
    {
        vector<pair<string, int>> teacher_scores;
        for (const auto &t : data.teachers)
        {
            if (find(t.eligible_courses.begin(), t.eligible_courses.end(), course.id) != t.eligible_courses.end())
            {
                int score = 0;
                auto it = t.course_pref.find(course.id);
                if (it != t.course_pref.end())
                    score = it->second;
                teacher_scores.emplace_back(t.id, score);
            }
        }
        sort(teacher_scores.begin(), teacher_scores.end(),
             [](const pair<string, int> &a, const pair<string, int> &b)
             {
                 return a.second > b.second;
             });
        for (auto &p : teacher_scores)
            course.Ij.push_back(p.first);
    }

    cout << "✅ Phase1 built from JSON: "
         << data.teachers.size() << " teachers, "
         << data.courses.size() << " courses, "
         << data.classrooms.days.size() << " days, "
         << data.classrooms.periods.size() << " periods.\n";

    return data;
}
