//-----------------------------------
// Copyright Pierric Gimmig 2013-2017
//-----------------------------------

#include "BpfTrace.h"
#include "LinuxUtils.h"
#include "CoreApp.h"
#include "Capture.h"
#include "OrbitModule.h"
#include "OrbitProcess.h"
#include "Utils.h"
#include <fstream>
#include <sstream>

//-----------------------------------------------------------------------------
BpfTrace::BpfTrace(Callback a_Callback)
{
    m_Callback = a_Callback ? a_Callback : [this](const std::string& a_Buffer)
    {
        CommandCallback(a_Buffer);
    };

    m_ScriptFileName = ws2s(Path::GetBasePath()) + "orbit.bt";
}

//-----------------------------------------------------------------------------
void BpfTrace::Start()
{
#if __linux__
    m_ExitRequested = false;
    m_TimerStacks.clear();
    if( !WriteBpfScript() )
        return;

    m_BpfCommand = std::string("bpftrace ") + m_ScriptFileName;
    m_Thread = std::make_shared<std::thread>
        ( &LinuxUtils::StreamCommandOutput
        , m_BpfCommand.c_str()
        , m_Callback
        , &m_ExitRequested );
    m_Thread->detach();
#endif
}

//-----------------------------------------------------------------------------
void BpfTrace::Stop()
{
    m_ExitRequested = true;
}

//-----------------------------------------------------------------------------
std::string BpfTrace::GetBpfScript()
{
    if (!m_Script.empty())
    {
        return m_Script;
    }

    std::stringstream ss;

    for (Function *func : Capture::GTargetProcess->GetFunctions())
    {
        if (func->IsSelected())
        {
            uint64_t virtual_address = (uint64_t)func->GetVirtualAddress();
            Capture::GSelectedFunctionsMap[func->m_Address] = func;

            ss << "   uprobe:" << func->m_Probe << R"({ printf("b )" << std::to_string(virtual_address) << R"( %u %lld\n%s\n\nd\n\n", tid, nsecs, ustack(perf)); })" << std::endl;
            ss << "uretprobe:" << func->m_Probe << R"({ printf("e )" << std::to_string(virtual_address) << R"( %u %lld\n", tid, nsecs); })" << std::endl;
        }
    }

    return ss.str();
}

//-----------------------------------------------------------------------------
bool BpfTrace::WriteBpfScript()
{
    std::string script = GetBpfScript();
    if (script.empty())
        return false;

    std::ofstream outFile;
    outFile.open(m_ScriptFileName);
    if (outFile.fail())
        return false;

    outFile << script;
    outFile.close();
    return true;
}

//-----------------------------------------------------------------------------
uint64_t BpfTrace::ProcessString(const std::string& a_String)
{
    auto hash = StringHash(a_String);
    if (m_StringMap.find(hash) == m_StringMap.end())
    {
        m_StringMap[hash] = a_String;
    }

    return hash;
}

//-----------------------------------------------------------------------------
void BpfTrace::CommandCallback(const std::string& a_Line)
{
    if (a_Line.empty() || a_Line == "\n")
        return;
    
    auto tokens = Tokenize(a_Line);
    
    const std::string& mode = tokens[0];
    bool isBegin = mode == "b";
    bool isEnd   = mode == "e";

    bool isStackLine = StartsWith(a_Line, "\t");
    bool isEndOfStack = a_Line == "d\n";

    if (!isBegin && !isEnd && !isStackLine && !isEndOfStack)
    {  
        if (StartsWith(a_Line, "Lost"))
        {
            PRINT(a_Line.c_str());
            return;
        }

        if (StartsWith(a_Line, "Attaching")) 
            return;

        // if the line does not start with one of the above,
        // we might have a broken line, e.g. due to a small buffer
        PRINT(Format("read unexpected line:%s\nthe buffer might be to small.", a_Line));
        return;
    }

    if (isStackLine) {
        const std::string& addressStr = LTrim(tokens[0]);
        uint64_t address = std::stoull(addressStr, nullptr, 16);

        std::string function = tokens[1];

        std::string moduleRaw = tokens[2];
        std::string module = Replace(moduleRaw.substr(1), ")\n", "");

        // TODO: this is copy&paste from LinuxPerf.cpp
        std::wstring moduleName = ToLower(Path::GetFileName(s2ws(module)));
        std::shared_ptr<Module> moduleFromName = Capture::GTargetProcess->GetModuleFromName( ws2s(moduleName) );
       
        if( moduleFromName )
        {
            uint64_t new_address = moduleFromName->ValidateAddress(address);
            address = new_address;
        }

        m_CallStack.m_Data.push_back(address);
        if( Capture::GTargetProcess && !Capture::GTargetProcess->HasSymbol(address))
        {
            auto symbol = std::make_shared<LinuxSymbol>();
            symbol->m_Name = function;
            symbol->m_Module = module;
            Capture::GTargetProcess->AddSymbol( address, symbol );
        }

        return;
    }

    if (isEndOfStack)
    {
        if ( m_CallStack.m_Data.size() ) {
            m_CallStack.m_Depth = (uint32_t)m_CallStack.m_Data.size();
            m_CallStack.m_ThreadId = atoi(m_LastThreadName.c_str());
            std::vector<Timer>& timers = m_TimerStacks[m_LastThreadName];
            if (timers.size())
            {
                Timer& timer = timers.back();
                timer.m_CallstackHash = m_CallStack.Hash();
                Capture::AddCallstack( m_CallStack );
            }
        }

        m_CallStack.m_Data.clear();
        m_LastThreadName = "";
        return;
    }

    const std::string& functionAddress  = tokens[1];
    const std::string& threadName       = tokens[2];
    const std::string& timestamp        = tokens[3];

    m_LastThreadName = threadName;

    if (isBegin)
    {
        Timer timer;
        timer.m_TID = atoi(threadName.c_str());
        uint64_t nanos = std::stoull(timestamp);
        timer.m_Start = nanos;
        timer.m_Depth = (uint8_t)m_TimerStacks[threadName].size();
        timer.m_FunctionAddress = std::stoull(functionAddress);
        m_TimerStacks[threadName].push_back(timer);
        return;
    }

    if (isEnd)
    {
        std::vector<Timer>& timers = m_TimerStacks[threadName];
        if (timers.size())
        {
            Timer& timer = timers.back();
            uint64_t nanos = std::stoull(timestamp);
            timer.m_End = nanos;
            GCoreApp->ProcessTimer(&timer, functionAddress);
            timers.pop_back();
        }
        return;
    }
}
