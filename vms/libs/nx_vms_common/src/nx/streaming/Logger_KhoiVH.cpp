#include "./Logger_KhoiVH.h"
Logger* Logger::m_instance = nullptr;
Logger::Logger()
{
}

Logger::~Logger()
{
}

void Logger::writeLogServer(std::string func, std::string logText)
{
    #if 0
    std::ofstream file;
    time_t now = time(0);
    std::string dt = ctime(&now);
    // convert now to string form

    file.open("LOG_KHOIVH.log", std::ios_base::app);

    file << dt + "-" + func + ":" + logText;
    file << "\n\n";
    #endif
}

Logger* Logger::instance()
{
    if (m_instance == nullptr) {
        m_instance = new Logger();
    }
    return m_instance;
}
