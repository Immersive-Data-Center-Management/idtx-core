/**
 * @file IDTXCoreLogger.h
 * @brief Specialization of the genereic Logger class to pipe the logging output to the console
 * 
 **/
#pragma once

#include <iostream>
#include <exception>

#include <idtx/utils/Logger.h>

namespace idtx {
namespace utils
{
    class IDTXCoreLogger : public ILogger
    {
    public:
        IDTXCoreLogger() = default;
        ~IDTXCoreLogger() override = default;
        
        void log(std::string_view formatted_line, LogLevel level) override
        {
            auto line = std::string(formatted_line);
            switch (level) {
            case LogLevel::Error:
                std::cerr << line << "\n";
                break;
            case LogLevel::Warn:
                std::cout << line << "\n";
                break;
            case LogLevel::Info:
                std::cout << line << "\n";
                break;
            case LogLevel::Debug:
                std::cout << line << "\n";
                break;
            }
        }
    };
}
}
