#pragma once

#include <drogon/HttpController.h>

class TeacherSchedulerController : public drogon::HttpController<TeacherSchedulerController> {
public:
    METHOD_LIST_BEGIN
    // POST /schedule
    ADD_METHOD_TO(TeacherSchedulerController::schedule, "/schedule", drogon::Post);
    // GET /health
    ADD_METHOD_TO(TeacherSchedulerController::health, "/health", drogon::Get);
    METHOD_LIST_END

    void schedule(const drogon::HttpRequestPtr &req,
                  std::function<void (const drogon::HttpResponsePtr &)> &&callback);

    void health(const drogon::HttpRequestPtr &req,
                std::function<void (const drogon::HttpResponsePtr &)> &&callback);
};
