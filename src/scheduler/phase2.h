#pragma once
#include "phase1.h"
#include <vector>
using namespace std;

struct InitialSolution {
    struct Assignment {
        string teacher_id;
        string course_id;
        string section_id;
        string day;
        string period;
        string classroom_id; // lớp học được phân bổ
        
        // Metadata phục vụ Phase 3
        string initial_teacher;    // Giáo viên ban đầu từ Phase 2 (để tính stability penalty)
        string initial_timeslot;   // Timeslot ban đầu từ Phase 2 (format: "day|period", để tính stability penalty)
    };

    std::vector<Assignment> assignments;
};

/**
 * Construct initial solution với random seed để tăng diversity
 * 
 * Phase 2 theo Gunawan et al. (2007):
 * - CHỈ sinh nghiệm feasible
 * - KHÔNG tối ưu soft constraints
 * - KHÔNG tạo bias thời gian/phòng/giáo viên
 * - Kết thúc ngay khi tìm được nghiệm feasible
 * 
 * @param data Problem data
 * @param random_seed Random seed để tạo nghiệm khác nhau
 * @param shuffle_sections Nếu true, shuffle thứ tự sections (future use)
 * @param shuffle_teachers Nếu true, shuffle thứ tự teachers (future use)
 * @param shuffle_timeslots Nếu true, shuffle thứ tự timeslots (future use)
 */
InitialSolution construct_initial_solution(
    const ProblemData &data,
    int random_seed = 0,
    bool shuffle_sections = false,
    bool shuffle_teachers = false,
    bool shuffle_timeslots = false);

/**
 * Generate pool of diverse feasible solutions
 * 
 * Chạy Phase 2 K lần với random seed khác nhau để sinh nghiệm đa dạng.
 * Chỉ giữ nghiệm feasible và đủ khác biệt.
 * 
 * @param data Problem data
 * @param K Số lần chạy Phase 2 (default: 8)
 * @param min_diversity Ngưỡng diversity tối thiểu (default: -1 = auto-compute based on problem size)
 *                      - Bài nhỏ (≤10 sections): 0.15
 *                      - Bài vừa (11-30 sections): 0.1
 *                      - Bài lớn (>30 sections): 0.05
 * @param max_solutions Số nghiệm tối đa trong pool (default: 10)
 * @return Vector các InitialSolution đa dạng
 */
vector<InitialSolution> generate_solution_pool(
    const ProblemData &data,
    int K = 8,
    double min_diversity = -1.0,
    int max_solutions = 10);

/**
 * Select N most diverse solutions from pool
 * 
 * @param pool Pool of solutions
 * @param N Số nghiệm cần chọn
 * @return Vector N nghiệm đa dạng nhất
 */
vector<InitialSolution> select_diverse_solutions(
    const vector<InitialSolution> &pool,
    int N);

InitialSolution assign_rooms_phase2(const ProblemData &data, const InitialSolution &initial);

/**
 * Đánh giá nhanh nghiệm Phase 2 (trước khi đưa sang Phase 3)
 * 
 * Chỉ tính:
 * - Workload imbalance rất thô (standard deviation của số periods/teacher)
 * - Timeslot distribution (độ phân tán của lịch)
 * 
 * Dùng để loại nghiệm quá tệ trước khi tốn thời gian chạy Phase 3.
 * 
 * @param sol Initial solution từ Phase 2
 * @param data Problem data
 * @return Score [0, 1] với 1 = tốt nhất, 0 = tệ nhất
 */
double evaluate_phase2_solution_quick(
    const InitialSolution &sol,
    const ProblemData &data);

/**
 * Kiểm tra xem nghiệm Phase 2 có đủ tốt để đưa sang Phase 3 không
 * 
 * @param sol Initial solution
 * @param data Problem data
 * @param min_score Ngưỡng score tối thiểu (default: 0.3)
 * @return true nếu nghiệm đủ tốt
 */
bool is_phase2_solution_good_enough(
    const InitialSolution &sol,
    const ProblemData &data,
    double min_score = 0.3);