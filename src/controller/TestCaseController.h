#pragma once

#include <drogon/HttpController.h>

class TestCaseController : public drogon::HttpController<TestCaseController> {
public:
    METHOD_LIST_BEGIN
    // POST /testcase/run
    ADD_METHOD_TO(TestCaseController::runTestCase, "/testcase/run", drogon::Post);
    // POST /testcase/run-cpsat
    ADD_METHOD_TO(TestCaseController::runTestCaseCPSat, "/testcase/run-cpsat", drogon::Post);
    METHOD_LIST_END

    void runTestCase(const drogon::HttpRequestPtr &req,
                    std::function<void (const drogon::HttpResponsePtr &)> &&callback);
    
    void runTestCaseCPSat(const drogon::HttpRequestPtr &req,
                         std::function<void (const drogon::HttpResponsePtr &)> &&callback);
};
