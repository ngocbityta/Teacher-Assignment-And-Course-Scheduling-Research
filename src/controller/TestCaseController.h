#pragma once

#include <drogon/HttpController.h>

class TestCaseController : public drogon::HttpController<TestCaseController> {
public:
    METHOD_LIST_BEGIN
    // POST /testcase/run
    ADD_METHOD_TO(TestCaseController::runTestCase, "/testcase/run", drogon::Post);
    METHOD_LIST_END

    void runTestCase(const drogon::HttpRequestPtr &req,
                    std::function<void (const drogon::HttpResponsePtr &)> &&callback);
};
