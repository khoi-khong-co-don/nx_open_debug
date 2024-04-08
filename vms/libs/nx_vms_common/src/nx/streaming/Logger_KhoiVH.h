#ifndef LOGGER_KHOIVH_H
#define LOGGER_KHOIVH_H
#include <iostream>
#include <fstream>
#include <string>
#include <ctime>

#define LOG_KhoiVH(var) Logger::instance()->writeLogServer(__func__, var)
class Logger
{
public:
    explicit Logger();
    virtual ~Logger();

    static Logger* instance();

    void writeLogServer(std::string func = "[Funtion]", std::string logText = "\n");
private:
    static Logger* m_instance;

};
#endif // LOGGER_KHOIVH_H
