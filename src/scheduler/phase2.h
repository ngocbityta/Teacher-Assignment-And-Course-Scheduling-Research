#pragma once
#include "phase1.h"
using namespace std;
struct InitialSolution {
    struct Assignment {
        string teacher_id;
        string course_id;
        string section_id;
        string day;
        string period;
        string classroom_id; // lớp học được phân bổ
    };

    std::vector<Assignment> assignments;
};

// Hàm xây dựng phương án khởi đầu bằng Integer Programming
InitialSolution construct_initial_solution(const ProblemData &data);