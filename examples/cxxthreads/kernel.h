//
// kernel.h — circle-libsdl2 C++ threading test
//
#ifndef _kernel_h
#define _kernel_h

#include <circle/actled.h>
#include <circle/koptions.h>
#include <circle/devicenameservice.h>
#include <circle/serial.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/timer.h>
#include <circle/logger.h>
#include <circle/types.h>
#include <circle/multicore.h>
#include <circle/memory.h>
#include <circle/sched/scheduler.h>

enum TShutdownMode
{
    ShutdownNone,
    ShutdownHalt,
    ShutdownReboot
};

// The secondary cores. Core 1 stands in for the application core every port
// runs its game on: no scheduler there, and until this library supplied one,
// no usable C++ threading runtime either.
class CTestCores : public CMultiCoreSupport
{
public:
    CTestCores(void) : CMultiCoreSupport(CMemorySystem::Get()) {}
    void Run(unsigned nCore) override;
};

class CKernel
{
public:
    CKernel(void);

    boolean Initialize(void);
    TShutdownMode Run(void);

private:
    // no CScreenDevice: this is a test of the C++ runtime, not of the picture
    CActLED             m_ActLED;
    CKernelOptions      m_Options;
    CDeviceNameService  m_DeviceNameService;
    CSerialDevice       m_Serial;
    CExceptionHandler   m_ExceptionHandler;
    CInterruptSystem    m_Interrupt;
    CTimer              m_Timer;
    CLogger             m_Logger;

    // Declared before the cores are started, so it exists by the time core 0
    // is armed: arming core 0 is what starts the threading runtime's creator
    // task, and a task needs a scheduler to register with.
    CScheduler          m_Scheduler;

    CTestCores          m_Cores;
};

#endif
