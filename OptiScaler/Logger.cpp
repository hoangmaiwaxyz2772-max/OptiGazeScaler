#include "pch.h"
#include "Logger.h"
#include "Config.h"
#include <iostream>

#include "spdlog/async.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/callback_sink.h"
#include <include/spdlog_sink/debug_sink.h>

#include "Util.h"

static std::filesystem::path FallbackLogPath()
{
    wchar_t localAppData[MAX_PATH] {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);

    std::filesystem::path basePath;
    if (length > 0 && length < MAX_PATH)
        basePath = localAppData;
    else
        basePath = std::filesystem::temp_directory_path();

    auto logDir = basePath / L"OptiGazeScaler";
    std::error_code ec;
    std::filesystem::create_directories(logDir, ec);
    return logDir / L"OptiScaler_fallback.log";
}

static void PrepareFallbackLogger(const char* reason)
{
    try
    {
        if (spdlog::default_logger() != nullptr)
            spdlog::default_logger().reset();

        auto fallbackPath = FallbackLogPath();
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(fallbackPath, true);
        file_sink->set_level(spdlog::level::level_enum::trace);
        file_sink->set_pattern("[%H:%M:%S.%f] [%L] %v");

        auto fallbackLogger = std::make_shared<spdlog::logger>("fallback_file_logger", file_sink);
        fallbackLogger->set_level(spdlog::level::level_enum::trace);
        fallbackLogger->flush_on(spdlog::level::trace);
        spdlog::set_default_logger(fallbackLogger);

        spdlog::error("PrepareLogger failed: {}", reason != nullptr ? reason : "unknown error");
        spdlog::error("Fallback log path: {}", wstring_to_string(fallbackPath.wstring()));
        spdlog::error("Configured log path: {}", wstring_to_string(Config::Instance()->LogFileName.value_or_default()));
        spdlog::error("LogToFile: {}, LogToConsole: {}, LogToDebug: {}, LogToNGX: {}, LogLevel: {}",
                      Config::Instance()->LogToFile.value_or_default(),
                      Config::Instance()->LogToConsole.value_or_default(),
                      Config::Instance()->LogToDebug.value_or_default(),
                      Config::Instance()->LogToNGX.value_or_default(),
                      Config::Instance()->LogLevel.value_or_default());
        spdlog::error("Exe path: {}", wstring_to_string(Util::ExePath().wstring()));
        spdlog::error("DLL path: {}", wstring_to_string(Util::DllPath().wstring()));
    }
    catch (const std::exception& fallbackException)
    {
        std::string message = std::string("OptiScaler fallback logger failed: ") + fallbackException.what();
        OutputDebugStringA(message.c_str());
        OutputDebugStringA("\n");
    }
}

static bool InitializeConsole()
{
    // Allocate a console for this app
    if (!AllocConsole())
        return false;

    FILE* pFile;

    // Redirect STDIN if the console has an input handle
    if (GetStdHandle(STD_INPUT_HANDLE) != INVALID_HANDLE_VALUE)
    {
        if (freopen_s(&pFile, "CONIN$", "r", stdin) != 0)
            return false;
    }

    // Redirect STDOUT if the console has an output handle
    if (GetStdHandle(STD_OUTPUT_HANDLE) != INVALID_HANDLE_VALUE)
    {
        if (freopen_s(&pFile, "CONOUT$", "w", stdout) != 0)
            return false;
    }

    // Redirect STDERR if the console has an error handle
    if (GetStdHandle(STD_ERROR_HANDLE) != INVALID_HANDLE_VALUE)
    {
        if (freopen_s(&pFile, "CONOUT$", "w", stderr) != 0)
            return false;
    }

    // Clear the error state for each of the C++ standard streams
    std::cin.clear();
    std::cout.clear();
    std::cerr.clear();
    std::wcin.clear();
    std::wcout.clear();
    std::wcerr.clear();

    // Make C++ standard streams point to console as well.
    std::ios::sync_with_stdio();

    WaitForEnter();

    return true;
}

void WaitForEnter()
{
    if (Config::Instance()->DebugWait.value_or_default())
    {
        std::cout << "Press ENTER to continue..." << std::endl;
        std::cin.get();
    }
}

void PrepareLogger()
{
    try
    {
        if (spdlog::default_logger() != nullptr)
            spdlog::default_logger().reset();

        if (Config::Instance()->LogToConsole.value_or_default() || Config::Instance()->LogToFile.value_or_default() ||
            Config::Instance()->LogToNGX.value_or_default() || Config::Instance()->LogToDebug.value_or_default())
        {
            if (Config::Instance()->OpenConsole.value_or_default())
                InitializeConsole();

            std::shared_ptr<spdlog::logger> shared_logger = nullptr;

            if (Config::Instance()->LogAsync.value_or_default())
            {
                // Set the queue size for asynchronous logging
                spdlog::init_thread_pool(8192, Config::Instance()->LogAsyncThreads.value_or_default());
            }

            std::vector<spdlog::sink_ptr> sinks;

            if (Config::Instance()->LogToDebug.value_or_default())
            {
                auto debug_sink = std::make_shared<spdlog::sinks::debug_sink_mt>();
                debug_sink->set_level(spdlog::level::level_enum::trace);

#ifdef LOG_ASYNC
                debug_sink->set_pattern("%H:%M:%S.%f\t%L\t%v");
#else
                debug_sink->set_pattern("[%H:%M:%S.%f] [%L] %v");
                // file_sink->set_pattern("[%H:%M:%S.%f] [thread %t] [%L] %v");
#endif // LOG_ASYNC

                sinks.push_back(debug_sink);
            }

            if (Config::Instance()->LogToConsole.value_or_default())
            {
                auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
                console_sink->set_level(spdlog::level::level_enum::info);
                console_sink->set_pattern("[%H:%M:%S.%f] [%L] %v");

                sinks.push_back(console_sink);
            }

            if (Config::Instance()->LogToFile.value_or_default())
            {
                auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                    Config::Instance()->LogFileName.value_or_default(), true);
                file_sink->set_level(spdlog::level::level_enum::trace);
#ifdef LOG_ASYNC
                file_sink->set_pattern("%H:%M:%S.%f\t%L\t%v");
#else
                file_sink->set_pattern("[%H:%M:%S.%f] [%L] %v");
                // file_sink->set_pattern("[%H:%M:%S.%f] [thread %t] [%L] %v");
#endif // LOG_ASYNC

                sinks.push_back(file_sink);
            }

            auto callback_sink = std::make_shared<spdlog::sinks::callback_sink_mt>(
                [](const spdlog::details::log_msg& msg)
                {
                    if (Config::Instance()->LogToNGX.value_or_default() &&
                        State::Instance().NVNGX_Logger.LoggingCallback != nullptr &&
                        State::Instance().NVNGX_Logger.MinimumLoggingLevel != NVSDK_NGX_LOGGING_LEVEL_OFF &&
                        (State::Instance().NVNGX_Logger.MinimumLoggingLevel == NVSDK_NGX_LOGGING_LEVEL_VERBOSE ||
                         msg.level >= spdlog::level::info))
                    {
                        auto message = (char*) msg.payload.data();
                        State::Instance().NVNGX_Logger.LoggingCallback(message, NVSDK_NGX_LOGGING_LEVEL_ON,
                                                                       NVSDK_NGX_Feature_SuperSampling);
                    }
                });

            callback_sink->set_level(spdlog::level::level_enum::trace);
            callback_sink->set_pattern("[%H:%M:%S.%f] [%L] %v");

            sinks.push_back(callback_sink);

            if (Config::Instance()->LogAsync.value_or_default())
            {
                shared_logger =
                    std::make_shared<spdlog::async_logger>("multi_sink_logger", sinks.begin(), sinks.end(),
                                                           spdlog::thread_pool(), spdlog::async_overflow_policy::block);
            }
            else
            {
                spdlog::logger logger("multi_sink", sinks.begin(), sinks.end());
                shared_logger = std::make_shared<spdlog::logger>(logger);
            }

            shared_logger->set_level((spdlog::level::level_enum) Config::Instance()->LogLevel.value_or_default());
            shared_logger->flush_on(spdlog::level::trace);

            spdlog::set_default_logger(shared_logger);
            spdlog::info("Logger initialized. Log file: {}",
                         wstring_to_string(Config::Instance()->LogFileName.value_or_default()));
        }
    }
    catch (const spdlog::spdlog_ex& ex)
    {
        std::cerr << ex.what() << std::endl;
        PrepareFallbackLogger(ex.what());
    }
    catch (const std::exception& ex)
    {
        std::cerr << ex.what() << std::endl;
        PrepareFallbackLogger(ex.what());
    }
}

void CloseLogger()
{
    spdlog::default_logger()->flush();
    spdlog::shutdown();
}
