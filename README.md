# Teacher Scheduler

Đây là dự án C++ dùng để lập lịch giảng viên. Hiện tại dự án chỉ hỗ trợ **macOS**, chưa phát triển cross-platform.

## Yêu cầu

- macOS
- Homebrew
- C++17 compiler
- Thư viện:
  - OR-Tools
  - nlohmann_json
 
## Build dự án

```
rm -rf build

cmake -S . -B build

cmake --build build
```

##  Chạy chương trình

``` ./build/bin/teacher_scheduler ```

## API endpoints

Ứng dụng sử dụng Drogon, expose các REST endpoint sau:

### 1. `POST /schedule`

**Chức năng**: Nhận dữ liệu đầu vào (giáo viên, môn học, lớp học) và trả về thời khóa biểu tối ưu.

- **Request body**: JSON với cấu trúc:

```json
{
  "classrooms": {
    "days": ["Mon", "Tue", "Wed", "Thu", "Fri"],
    "periods": ["1", "2", "3"],
    "classrooms": [
      { "id": "R101", "capacity": 50 },
      { "id": "R102", "capacity": 40 }
    ]
    // (tuỳ chọn, để tương thích cũ)
    // "classrooms_per_slot": { "Mon": { "1": 3, "2": 3, "3": 3 }, ... }
  },
  "teachers": [
    {
      "id": "T1",
      "name": "Nguyen Van A",
      "max_courses": 2,
      "eligible_courses": ["MATH101", "CS102"],
      "course_preferences": {
        "MATH101": 8,
        "CS102": 9
      },
      "day_time_preferences": {
        "Mon": { "1": 9, "2": 8, "3": 6 },
        "Tue": { "1": 7, "2": 8, "3": 5 }
      }
    }
  ],
  "courses": [
    {
      "id": "MATH101",
      "name": "Calculus I",
      "min_teachers": 1,
      "max_teachers": 2,
      "sections": [
        { "id": "S1", "required_periods": 2, "required_seats": 40 },
        { "id": "S2", "required_periods": 2, "required_seats": 35 }
      ]
    }
  ]
}
```

- **Response**:

```json
{
  "status": "success",
  "solution": {
    "objective_value": 1234,
    "assignments": [
      {
        "teacher_id": "T1",
        "course_id": "MATH101",
        "section_id": "S1",
        "day": "Mon",
        "period": "1",
        "classroom_id": "R101"
      }
    ]
  }
}
```

Nếu lỗi, trường `status` sẽ là `"error"` và có thêm `message`.

> Tham khảo file ví dụ: `example-data/request.json`.

### 2. `POST /testcase/run`

**Chức năng**: Chạy một test case có sẵn trong thư mục `testcases/` và trả về thống kê + kết quả + thời gian chạy. Endpoint này **chỉ chạy khi được gọi**, không ảnh hưởng thời gian build.

- **Request body**:

```json
{
  "test_case_name": "test_case_01_balanced.json"
}
```

Tên file phải tồn tại trong thư mục `testcases/`.

- **Response (success)**:

```json
{
  "status": "success",
  "test_case_name": "test_case_01_balanced.json",
  "statistics": {
    "num_teachers": 7,
    "num_classrooms": 4,
    "num_courses": 15,
    "num_sections": 25,
    "num_assignments": 25
  },
  "timing": {
    "total_time_ms": 1234,
    "total_time_seconds": 1.234,
    "phase1_init_ms": 10,
    "phase2_initial_solution_ms": 500,
    "phase3_optimization_ms": 724
  },
  "solution": {
    "objective_value": 1234,
    "assignments": [
      {
        "teacher_id": "T1",
        "course_id": "MATH101",
        "section_id": "S1",
        "day": "Mon",
        "period": "1",
        "classroom_id": "R101"
      }
    ]
  }
}
```

Khi lỗi (file không tồn tại, JSON sai, lỗi runtime, …) sẽ trả:

```json
{
  "status": "error",
  "message": "mô tả lỗi",
  "test_case_name": "tên file (nếu có)",
  "timing": {
    "total_time_ms": 50
  }
}
```

> Các test case mẫu đã được chuẩn bị trong thư mục `testcases/` (10 bộ dữ liệu khác nhau, bao phủ nhiều kịch bản).
