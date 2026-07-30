//
// kernel.h — circle-libsdl2 video-restart cycle test
//
// The core layout is the product's: core 0 the hardware core, core 1 the
// application, core 2 the elected presentation worker. The test exists to
// exercise teardown and re-creation of the video and audio devices while a
// presentation worker is live, so the split is not optional here.
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
#include <circle/sched/scheduler.h>
#include <circle/cputhrottle.h>
#include <circle/multicore.h>
#include <circle/memory.h>
#include <circle/types.h>

enum TShutdownMode
{
    ShutdownNone,
    ShutdownHalt,
    ShutdownReboot
};

// Secondary-core dispatch. A core handed no role parks: returning from Run()
// would leave it executing whatever follows.
class CSplitCores : public CMultiCoreSupport
{
public:
    CSplitCores(void) : CMultiCoreSupport(CMemorySystem::Get()) {}
    void Run(unsigned nCore) override;
};

class CKernel
{
public:
    CKernel(void);

    boolean Initialize(void);
    TShutdownMode Run(void);

private:
    // No CScreenDevice: the SDL window owns the display.
    CActLED             m_ActLED;
    CKernelOptions      m_Options;
    CDeviceNameService  m_DeviceNameService;
    CSerialDevice       m_Serial;
    CExceptionHandler   m_ExceptionHandler;
    CInterruptSystem    m_Interrupt;
    CTimer              m_Timer;
    CLogger             m_Logger;
    CScheduler          m_Scheduler;
    // Circle boots at idle clock, and the shim reaches this object through
    // CCPUThrottle::Get(), which asserts rather than returning null — so a
    // kernel without one dies on the first event pump.
    CCPUThrottle        m_CPUThrottle;
    CSplitCores         m_Cores;
};

#endif
