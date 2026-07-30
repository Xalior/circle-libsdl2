//
// kernel.cpp — dispinfo: a serial-only probe of what the Raspberry Pi
// firmware reports about the attached display, and of what Circle's
// CBcmFrameBuffer returns for a set of allocation requests.
//
// It logs raw values and nothing else. No screen output, no SDL, no
// interpretation: the numbers are read somewhere else.
//
// Every mailbox property tag it uses is declared in
// circle/include/circle/bcmpropertytags.h; no tag outside that header is
// invented here. The framebuffer allocations use CBcmFrameBuffer with the
// same argument shape the SDL shim uses.
//
#include "kernel.h"
#include <circle/bcmpropertytags.h>
#include <circle/bcmframebuffer.h>
#include <circle/util.h>
#include <circle/string.h>

static const char From[] = "dispinfo";

// ---------------------------------------------------------------------------
// raw logging helpers
// ---------------------------------------------------------------------------

// The response header of every tag, so a caller can see the firmware's own
// nValueLength (bit 31 set marks a response, see TPropertyTag in
// bcmpropertytags.h) alongside the payload.
static void LogTagHeader(const char *pWhat, boolean bOK, const TPropertyTag *pTag)
{
    CLogger::Get()->Write(From, LogNotice,
                          "%s: GetTag=%s tagid=0x%08X valbufsize=%u vallen=0x%08X",
                          pWhat, bOK ? "TRUE" : "FALSE",
                          (unsigned) pTag->nTagId,
                          (unsigned) pTag->nValueBufSize,
                          (unsigned) pTag->nValueLength);
}

static void LogHexBlock(const char *pPrefix, const u8 *pData, unsigned nLen)
{
    for (unsigned i = 0; i < nLen; i += 16)
    {
        CString Line;
        CString Byte;
        Line.Format("%s %03u:", pPrefix, i);
        for (unsigned j = 0; j < 16 && i + j < nLen; j++)
        {
            Byte.Format(" %02X", (unsigned) pData[i + j]);
            Line.Append(Byte);
        }
        CLogger::Get()->Write(From, LogNotice, "%s", (const char *) Line);
    }
}

// ---------------------------------------------------------------------------
// board and boot context — raw, so the capture identifies itself
// ---------------------------------------------------------------------------

static void ProbeBoardContext(void)
{
    CBcmPropertyTags Tags;

    static const struct { u32 nTag; const char *pName; } Simple[] = {
        { PROPTAG_GET_FIRMWARE_REVISION, "PROPTAG_GET_FIRMWARE_REVISION" },
        { PROPTAG_GET_BOARD_MODEL,       "PROPTAG_GET_BOARD_MODEL"       },
        { PROPTAG_GET_BOARD_REVISION,    "PROPTAG_GET_BOARD_REVISION"    },
    };

    for (unsigned i = 0; i < sizeof Simple / sizeof Simple[0]; i++)
    {
        TPropertyTagSimple Tag;
        memset(&Tag, 0, sizeof Tag);
        boolean bOK = Tags.GetTag(Simple[i].nTag, &Tag, sizeof Tag);
        LogTagHeader(Simple[i].pName, bOK, &Tag.Tag);
        CLogger::Get()->Write(From, LogNotice, "%s: value=0x%08X (%u)",
                              Simple[i].pName, (unsigned) Tag.nValue,
                              (unsigned) Tag.nValue);
    }

    static const struct { u32 nTag; const char *pName; } Mem[] = {
        { PROPTAG_GET_ARM_MEMORY, "PROPTAG_GET_ARM_MEMORY" },
        { PROPTAG_GET_VC_MEMORY,  "PROPTAG_GET_VC_MEMORY"  },
    };

    for (unsigned i = 0; i < sizeof Mem / sizeof Mem[0]; i++)
    {
        TPropertyTagMemory Tag;
        memset(&Tag, 0, sizeof Tag);
        boolean bOK = Tags.GetTag(Mem[i].nTag, &Tag, sizeof Tag);
        LogTagHeader(Mem[i].pName, bOK, &Tag.Tag);
        CLogger::Get()->Write(From, LogNotice,
                              "%s: base=0x%08X size=0x%08X (%u)",
                              Mem[i].pName, (unsigned) Tag.nBaseAddress,
                              (unsigned) Tag.nSize, (unsigned) Tag.nSize);
    }

    // The firmware's own view of the boot command line, which is what
    // Circle's CKernelOptions parses (see doc/cmdline.txt).
    TPropertyTagCommandLine *pCmdLine = new TPropertyTagCommandLine;
    memset(pCmdLine, 0, sizeof *pCmdLine);
    boolean bOK = Tags.GetTag(PROPTAG_GET_COMMAND_LINE, pCmdLine, sizeof *pCmdLine);
    LogTagHeader("PROPTAG_GET_COMMAND_LINE", bOK, &pCmdLine->Tag);
    pCmdLine->String[sizeof pCmdLine->String - 1] = '\0';
    CLogger::Get()->Write(From, LogNotice, "PROPTAG_GET_COMMAND_LINE: \"%s\"",
                          (const char *) pCmdLine->String);
    delete pCmdLine;
}

// ---------------------------------------------------------------------------
// the display tags
// ---------------------------------------------------------------------------

static u32 ProbeNumDisplays(void)
{
    CBcmPropertyTags Tags;

    TPropertyTagSimple Tag;
    memset(&Tag, 0, sizeof Tag);
    boolean bOK = Tags.GetTag(PROPTAG_GET_NUM_DISPLAYS, &Tag, sizeof Tag);
    LogTagHeader("PROPTAG_GET_NUM_DISPLAYS", bOK, &Tag.Tag);
    CLogger::Get()->Write(From, LogNotice, "PROPTAG_GET_NUM_DISPLAYS: value=%u",
                          (unsigned) Tag.nValue);

    // Circle's own accessor, which falls back to 1 when the tag fails.
    CLogger::Get()->Write(From, LogNotice,
                          "CBcmFrameBuffer::GetNumDisplays() = %u",
                          CBcmFrameBuffer::GetNumDisplays());

    return bOK ? Tag.nValue : 0;
}

static void ProbeDimensions(const char *pWhen)
{
    CBcmPropertyTags Tags;

    TPropertyTagDisplayDimensions Dim;
    memset(&Dim, 0, sizeof Dim);
    boolean bOK = Tags.GetTag(PROPTAG_GET_DISPLAY_DIMENSIONS, &Dim, sizeof Dim);
    CString What;
    What.Format("PROPTAG_GET_DISPLAY_DIMENSIONS [%s]", pWhen);
    LogTagHeader((const char *) What, bOK, &Dim.Tag);
    CLogger::Get()->Write(From, LogNotice, "%s: width=%u height=%u",
                          (const char *) What, (unsigned) Dim.nWidth,
                          (unsigned) Dim.nHeight);
}

static void ProbePitch(const char *pWhen)
{
    CBcmPropertyTags Tags;

    TPropertyTagSimple Tag;
    memset(&Tag, 0, sizeof Tag);
    boolean bOK = Tags.GetTag(PROPTAG_GET_PITCH, &Tag, sizeof Tag);
    CString What;
    What.Format("PROPTAG_GET_PITCH [%s]", pWhen);
    LogTagHeader((const char *) What, bOK, &Tag.Tag);
    CLogger::Get()->Write(From, LogNotice, "%s: pitch=%u", (const char *) What,
                          (unsigned) Tag.nValue);
}

// Every EDID block the firmware will hand over, dumped byte for byte.
// TPropertyTagEDIDBlock carries the block number in, and a status plus 128
// bytes out; EDID_STATUS_SUCCESS is 0.
static void ProbeEDID(const char *pWhen)
{
    static const unsigned MaxBlocks = 8;

    for (unsigned nBlock = 0; nBlock < MaxBlocks; nBlock++)
    {
        CBcmPropertyTags Tags;

        TPropertyTagEDIDBlock EDID;
        memset(&EDID, 0, sizeof EDID);
        EDID.nBlockNumber = nBlock;

        boolean bOK = Tags.GetTag(PROPTAG_GET_EDID_BLOCK, &EDID, sizeof EDID, 4);

        CString What;
        What.Format("PROPTAG_GET_EDID_BLOCK [%s] block=%u", pWhen, nBlock);
        LogTagHeader((const char *) What, bOK, &EDID.Tag);
        CLogger::Get()->Write(From, LogNotice,
                              "%s: echoed-block=%u status=0x%08X",
                              (const char *) What,
                              (unsigned) EDID.nBlockNumber,
                              (unsigned) EDID.nStatus);

        CString Prefix;
        Prefix.Format("EDID[%s] b%u", pWhen, nBlock);
        LogHexBlock((const char *) Prefix, EDID.Block, sizeof EDID.Block);

        if (!bOK || EDID.nStatus != EDID_STATUS_SUCCESS)
        {
            CLogger::Get()->Write(From, LogNotice,
                                  "%s: stopping block walk here", (const char *) What);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// framebuffer allocations
// ---------------------------------------------------------------------------

// One allocation attempt, reported through every accessor bcmframebuffer.h
// exposes. Constructed with the same argument shape the SDL shim uses:
// CBcmFrameBuffer(w, h, 32, 0, 0, 0, bDoubleBuffered).
static void ProbeFrameBuffer(const char *pLabel, unsigned nWidth, unsigned nHeight,
                             unsigned nDepth, boolean bDoubleBuffered)
{
    CLogger::Get()->Write(From, LogNotice,
                          "---- FB CASE %s: request w=%u h=%u depth=%u double=%s ----",
                          pLabel, nWidth, nHeight, nDepth,
                          bDoubleBuffered ? "TRUE" : "FALSE");

    CBcmFrameBuffer *pFB = new CBcmFrameBuffer(nWidth, nHeight, nDepth,
                                               0, 0, 0, bDoubleBuffered);

    boolean bInit = pFB->Initialize();

    CLogger::Get()->Write(From, LogNotice, "FB CASE %s: Initialize=%s", pLabel,
                          bInit ? "TRUE" : "FALSE");
    CLogger::Get()->Write(From, LogNotice,
                          "FB CASE %s: GetWidth=%u GetHeight=%u GetVirtWidth=%u GetVirtHeight=%u",
                          pLabel, (unsigned) pFB->GetWidth(),
                          (unsigned) pFB->GetHeight(),
                          (unsigned) pFB->GetVirtWidth(),
                          (unsigned) pFB->GetVirtHeight());
    CLogger::Get()->Write(From, LogNotice,
                          "FB CASE %s: GetPitch=%u GetDepth=%u GetSize=%u (0x%08X) GetBuffer=0x%08X",
                          pLabel, (unsigned) pFB->GetPitch(),
                          (unsigned) pFB->GetDepth(),
                          (unsigned) pFB->GetSize(),
                          (unsigned) pFB->GetSize(),
                          (unsigned) pFB->GetBuffer());

    CString When;
    When.Format("after FB %s", pLabel);
    ProbeDimensions((const char *) When);
    ProbePitch((const char *) When);

    delete pFB;
}

// ---------------------------------------------------------------------------

CKernel::CKernel(void)
    // Serial device 0 is the GPIO14/15 header UART on every board. Named
    // explicitly because Circle's RASPPI >= 5 default (SERIAL_DEVICE_DEFAULT
    // = 10) is the Pi 5's dedicated debug connector, so taking the default
    // sends every log line somewhere nobody is listening.
    : m_Serial(0, FALSE, 0),
      m_Timer(&m_Interrupt),
      m_Logger(m_Options.GetLogLevel(), &m_Timer)
{
    m_ActLED.Blink(3);
}

boolean CKernel::Initialize(void)
{
    boolean bOK = TRUE;
    if (bOK) bOK = m_Serial.Initialize(115200);
    if (bOK) bOK = m_Logger.Initialize(&m_Serial);
    if (bOK) bOK = m_Interrupt.Initialize();
    if (bOK) bOK = m_Timer.Initialize();
    return bOK;
}

TShutdownMode CKernel::Run(void)
{
    m_Logger.Write(From, LogNotice, "===== dispinfo probe begin =====");
    m_Logger.Write(From, LogNotice, "RASPPI=%d AARCH=%d", RASPPI, AARCH);

    ProbeBoardContext();

    m_Logger.Write(From, LogNotice, "----- display tags, before any allocation -----");
    ProbeNumDisplays();
    ProbeDimensions("virgin");
    ProbePitch("virgin");
    ProbeEDID("virgin");

    m_Logger.Write(From, LogNotice, "----- framebuffer allocations -----");

    // The shim's exact request first, on untouched firmware state.
    ProbeFrameBuffer("A 1280x720x32 double", 1280, 720, 32, TRUE);
    ProbeFrameBuffer("B 1280x720x32 single", 1280, 720, 32, FALSE);
    ProbeFrameBuffer("C 640x480x32 single", 640, 480, 32, FALSE);

    // Width and height of zero is Circle's "no size requested": the
    // CBcmFrameBuffer constructor fills them in from
    // PROPTAG_GET_DISPLAY_DIMENSIONS, clamping to 640x480 outside
    // 640..4096 x 480..2160.
    ProbeFrameBuffer("D default-size x32 single", 0, 0, 32, FALSE);

    // The same two cases repeated, so any dependence on what was allocated
    // before them is visible inside a single capture.
    ProbeFrameBuffer("D2 default-size x32 single (repeat)", 0, 0, 32, FALSE);
    ProbeFrameBuffer("A2 1280x720x32 double (repeat)", 1280, 720, 32, TRUE);

    m_Logger.Write(From, LogNotice, "----- display tags, after all allocations -----");
    ProbeNumDisplays();
    ProbeDimensions("final");
    ProbePitch("final");
    ProbeEDID("final");

    m_Logger.Write(From, LogNotice, "===== dispinfo probe end =====");

    return ShutdownHalt;
}
