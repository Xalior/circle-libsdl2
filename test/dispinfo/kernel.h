//
// kernel.h — dispinfo probe: what the firmware reports about the display
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

enum TShutdownMode
{
    ShutdownNone,
    ShutdownHalt,
    ShutdownReboot
};

class CKernel
{
public:
    CKernel(void);

    boolean Initialize(void);
    TShutdownMode Run(void);

private:
    // No CScreenDevice and no SDL: the probe writes to serial only, so
    // nothing in this kernel touches the framebuffer except the explicit
    // CBcmFrameBuffer allocations under test. CInterruptSystem is present
    // because CBcmFrameBuffer builds a CDMAChannel when Circle's default
    // SCREEN_DMA_BURST_LENGTH is in force, and that reaches
    // CInterruptSystem::Get().
    CActLED             m_ActLED;
    CKernelOptions      m_Options;
    CDeviceNameService  m_DeviceNameService;
    CSerialDevice       m_Serial;
    CExceptionHandler   m_ExceptionHandler;
    CInterruptSystem    m_Interrupt;
    CTimer              m_Timer;
    CLogger             m_Logger;
};

#endif
