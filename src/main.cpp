#include <drogon/drogon.h>
using namespace drogon;

int main() {
    app().loadConfigFile("config.json");
    LOG_INFO << "Starting Teacher Scheduler Application at port 8081";
    app().run();
    return 0;
}
