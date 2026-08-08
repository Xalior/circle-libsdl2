//
// diskcache.cpp — the pool, the bookkeeping, and the report.
//
// See diskcache.h for what this class is and why each of its three decisions
// is the way it is. Two rules govern every line below.
//
// The first: the real device's result is what the caller gets. A read this
// class answers from memory returns exactly the bytes the card would have
// returned, and a read it cannot answer is passed straight through. A write
// reaches the card before this class returns, always.
//
// The second: the read path allocates nothing and scans nothing. Every lookup
// goes through the hash index, every victim comes from a fixed-size random
// sample, and the pool itself is never walked. That is what makes a larger
// pool cost memory and nothing else.
//
#include "diskcache.h"

#include <circle/devicenameservice.h>
#include <circle/logger.h>
#include <circle/memory.h>
#include <circle/timer.h>
#include <circle/util.h>

static const char From[] = "diskcache";

#define DISKCACHE_NO_SLOT	0xFFFFFFFFU
#define DISKCACHE_NO_SECTOR	((u64) -1)

// A pool larger than this is refused outright rather than allowed to fail in
// some less obvious way later. It is far beyond anything this is for.
#define DISKCACHE_MAX_KB	(1024 * 1024)

// Sector numbers are spread across the index by multiplying with the 64-bit
// golden-ratio constant and keeping the high bits, which mixes the low bits
// that consecutive sectors differ in.
static inline u32 HashSector (u64 nSector)
{
	return (u32) ((nSector * 0x9E3779B97F4A7C15ULL) >> 32);
}

// Percentages are kept as tenths of a percent and printed as two integers.
// There is no floating point anywhere in this file: this code sits in front of
// every disk access and the report is the only place a fraction is wanted.
static inline unsigned PercentTenths (u64 nPart, u64 nWhole)
{
	if (nWhole == 0)
	{
		return 0;
	}
	return (unsigned) ((nPart * 1000 + nWhole / 2) / nWhole);
}

static inline unsigned MeanTenths (u64 nTotal, u64 nCount)
{
	if (nCount == 0)
	{
		return 0;
	}
	return (unsigned) ((nTotal * 10 + nCount / 2) / nCount);
}

CDiskCacheDevice::CDiskCacheDevice (const char *pDeviceName)
:	m_pDeviceName (pDeviceName),
	m_pDevice (0),
	m_ullOffset (0),
	m_pData (0),
	m_pSlot (0),
	m_pBucket (0),
	m_pHistory (0),
	m_nSlots (0),
	m_nSlotsUsed (0),
	m_nBucketMask (0),
	m_nHistoryMask (0),
	m_nPoolKB (0),
	m_nOverheadKB (0),
	m_nAccessCounter (0),
	m_nRandomState (0x123456789ABCDEFULL),
	m_nCacheHits (0),
	m_nHitSectors (0),
	m_nAdmissions (0),
	m_nEvictions (0),
	m_nFirstSightings (0),
	m_nWriteUpdates (0),
	m_nNextExpected (0),
	m_bHavePrevious (FALSE),
	m_nSequential (0),
	m_nJumpForward (0),
	m_nJumpBackward (0),
	m_ullLastReportTicks (0),
	m_ullFirstTicks (0),
	m_bStarted (FALSE),
	m_bLegendPrinted (FALSE),
	m_nPrevReadRequests (0),
	m_nPrevReadSectors (0),
	m_nPrevWriteRequests (0),
	m_nPrevWriteSectors (0)
{
	ResetDir (&m_Read);
	ResetDir (&m_Write);

	memset (m_nAgeBucket, 0, sizeof m_nAgeBucket);
	memset (m_nJumpBucket, 0, sizeof m_nJumpBucket);
}

CDiskCacheDevice::~CDiskCacheDevice (void)
{
	ReleasePool ();
	m_pDevice = 0;
}

void CDiskCacheDevice::ResetDir (TDirStats *pDir)
{
	memset (pDir, 0, sizeof *pDir);
	pDir->nMinMicros = ~0U;
}

// ---------------------------------------------------------------------------
// Getting in the way, and getting the memory
// ---------------------------------------------------------------------------

boolean CDiskCacheDevice::Install (void)
{
	CDeviceNameService *pNames = CDeviceNameService::Get ();
	if (pNames == 0)
	{
		return FALSE;
	}

	CDevice *pReal = pNames->GetDevice (m_pDeviceName, TRUE);
	if (pReal == 0 || pReal == this)
	{
		return FALSE;
	}

	// Unregister the name and register this object under it. RemoveDevice on
	// the name service drops the name entry only — the real device object is
	// left alone, still initialised, and is what every call below reaches.
	pNames->RemoveDevice (m_pDeviceName, TRUE);
	pNames->AddDevice (m_pDeviceName, this, TRUE);

	m_pDevice = pReal;

	m_ullFirstTicks = CTimer::GetClockTicks64 ();
	m_ullLastReportTicks = m_ullFirstTicks;
	m_bStarted = TRUE;

	return TRUE;
}

unsigned CDiskCacheDevice::NextPowerOfTwo (unsigned nValue)
{
	unsigned n = 1;
	while (n < nValue && n < 0x40000000U)
	{
		n <<= 1;
	}
	return n;
}

void CDiskCacheDevice::ReleasePool (void)
{
	if (m_pData != 0)	CMemorySystem::HeapFree (m_pData);
	if (m_pSlot != 0)	CMemorySystem::HeapFree (m_pSlot);
	if (m_pBucket != 0)	CMemorySystem::HeapFree (m_pBucket);
	if (m_pHistory != 0)	CMemorySystem::HeapFree (m_pHistory);

	m_pData = 0;
	m_pSlot = 0;
	m_pBucket = 0;
	m_pHistory = 0;
	m_nSlots = 0;
	m_nSlotsUsed = 0;
	m_nBucketMask = 0;
	m_nHistoryMask = 0;
	m_nPoolKB = 0;
	m_nOverheadKB = 0;
}

boolean CDiskCacheDevice::Configure (unsigned nKilobytes)
{
	CLogger *pLog = CLogger::Get ();

	ReleasePool ();

	if (nKilobytes == 0)
	{
		if (pLog != 0)
		{
			pLog->Write (From, LogNotice,
				     "cache off — every read reaches the card, and only the "
				     "counting is running");
		}
		return TRUE;
	}

	if (nKilobytes > DISKCACHE_MAX_KB)
	{
		if (pLog != 0)
		{
			pLog->Write (From, LogError,
				     "%u KB asked for, which is past the %u KB limit — cache off",
				     nKilobytes, (unsigned) DISKCACHE_MAX_KB);
		}
		return FALSE;
	}

	unsigned nSlots = nKilobytes * (1024 / DISKCACHE_SECTOR_SIZE);
	unsigned nBuckets = NextPowerOfTwo (nSlots);
	unsigned nHistory = NextPowerOfTwo (nSlots) * 2;

	size_t nData = (size_t) nSlots * DISKCACHE_SECTOR_SIZE;
	size_t nSlotBytes = (size_t) nSlots * sizeof (TSlot);
	size_t nBucketBytes = (size_t) nBuckets * sizeof (u32);
	size_t nHistoryBytes = (size_t) nHistory * sizeof (u32);
	size_t nOverhead = nSlotBytes + nBucketBytes + nHistoryBytes;

	// Leave the rest of the machine room to exist. The program this serves
	// has not made its own allocations yet — a game claims its heap when it
	// starts, long after this — so taking most of what is free here would
	// come back as a failure somewhere with no idea why. Refusing loudly and
	// running without a cache is the honest outcome: the operator picks a
	// smaller number and tries again.
	size_t nFree = CMemorySystem::Get ()->GetHeapFreeSpace (HEAP_LOW);
	if (nData + nOverhead > nFree / 4)
	{
		if (pLog != 0)
		{
			pLog->Write (From, LogError,
				     "%u KB pool needs %u KB in total and only %u KB of low heap "
				     "is free — cache off, ask for less",
				     nKilobytes,
				     (unsigned) ((nData + nOverhead) / 1024),
				     (unsigned) (nFree / 1024));
		}
		return FALSE;
	}

	// The low heap, named explicitly. On a board with more than a gigabyte
	// the excess is a separate heap this project does not use yet, and
	// staying low keeps one identical path on every board.
	m_pData = (u8 *) CMemorySystem::HeapAllocate (nData, HEAP_LOW);
	m_pSlot = (TSlot *) CMemorySystem::HeapAllocate (nSlotBytes, HEAP_LOW);
	m_pBucket = (u32 *) CMemorySystem::HeapAllocate (nBucketBytes, HEAP_LOW);
	m_pHistory = (u32 *) CMemorySystem::HeapAllocate (nHistoryBytes, HEAP_LOW);

	if (m_pData == 0 || m_pSlot == 0 || m_pBucket == 0 || m_pHistory == 0)
	{
		ReleasePool ();
		if (pLog != 0)
		{
			pLog->Write (From, LogError,
				     "%u KB pool refused by the low heap — cache off", nKilobytes);
		}
		return FALSE;
	}

	for (unsigned i = 0; i < nSlots; i++)
	{
		m_pSlot[i].nSector = DISKCACHE_NO_SECTOR;
		m_pSlot[i].nStamp = 0;
		m_pSlot[i].nHashNext = DISKCACHE_NO_SLOT;
		m_pSlot[i].nReserved = 0;
	}
	memset (m_pBucket, 0xFF, nBucketBytes);		// every bucket empty
	memset (m_pHistory, 0, nHistoryBytes);		// nothing seen yet

	m_nSlots = nSlots;
	m_nSlotsUsed = 0;
	m_nBucketMask = nBuckets - 1;
	m_nHistoryMask = nHistory - 1;
	m_nPoolKB = nKilobytes;
	m_nOverheadKB = (unsigned) (nOverhead / 1024);

	// The victim sample has to be unpredictable to break a cyclic read
	// pattern, and the clock is the only thing that differs between boots.
	m_nRandomState = CTimer::GetClockTicks64 () | 1;

	if (pLog != 0)
	{
		pLog->Write (From, LogNotice,
			     "cache on: %u KB pool in %u blocks of %u bytes, %u KB of "
			     "bookkeeping, write-through",
			     nKilobytes, nSlots, (unsigned) DISKCACHE_SECTOR_SIZE,
			     m_nOverheadKB);
	}

	return TRUE;
}

// ---------------------------------------------------------------------------
// The pool
// ---------------------------------------------------------------------------

u8 *CDiskCacheDevice::SlotData (u32 nSlot) const
{
	return m_pData + (size_t) nSlot * DISKCACHE_SECTOR_SIZE;
}

u32 CDiskCacheDevice::Lookup (u64 nSector) const
{
	if (m_nSlots == 0)
	{
		return DISKCACHE_NO_SLOT;
	}

	u32 nSlot = m_pBucket[HashSector (nSector) & m_nBucketMask];
	while (nSlot != DISKCACHE_NO_SLOT)
	{
		if (m_pSlot[nSlot].nSector == nSector)
		{
			return nSlot;
		}
		nSlot = m_pSlot[nSlot].nHashNext;
	}

	return DISKCACHE_NO_SLOT;
}

void CDiskCacheDevice::IndexInsert (u32 nSlot)
{
	unsigned nBucket = HashSector (m_pSlot[nSlot].nSector) & m_nBucketMask;
	m_pSlot[nSlot].nHashNext = m_pBucket[nBucket];
	m_pBucket[nBucket] = nSlot;
}

// Chains are one or two entries long, because there is at least one bucket per
// slot, so this is a fixed cost in practice and never a walk of the pool.
void CDiskCacheDevice::IndexRemove (u32 nSlot)
{
	if (m_pSlot[nSlot].nSector == DISKCACHE_NO_SECTOR)
	{
		return;					// never indexed
	}

	unsigned nBucket = HashSector (m_pSlot[nSlot].nSector) & m_nBucketMask;
	u32 nThis = m_pBucket[nBucket];
	u32 nPrev = DISKCACHE_NO_SLOT;

	while (nThis != DISKCACHE_NO_SLOT && nThis != nSlot)
	{
		nPrev = nThis;
		nThis = m_pSlot[nThis].nHashNext;
	}

	if (nThis != nSlot)
	{
		return;
	}

	if (nPrev == DISKCACHE_NO_SLOT)
	{
		m_pBucket[nBucket] = m_pSlot[nSlot].nHashNext;
	}
	else
	{
		m_pSlot[nPrev].nHashNext = m_pSlot[nSlot].nHashNext;
	}

	m_pSlot[nSlot].nHashNext = DISKCACHE_NO_SLOT;
}

// xorshift64*, which is a handful of instructions and no state beyond one
// word. It does not need to be a good random number generator; it needs to be
// unpredictable enough that a repeating read pattern cannot fall into step
// with the choice of victim.
u32 CDiskCacheDevice::Random (unsigned nLimit)
{
	u64 x = m_nRandomState;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	m_nRandomState = x;

	u32 nValue = (u32) ((x * 0x2545F4914F6CDD1DULL) >> 32);

	// Scale into range by multiplying rather than dividing: no modulo, and no
	// bias worth the instructions it would cost to remove.
	return (u32) (((u64) nValue * nLimit) >> 32);
}

u32 CDiskCacheDevice::ClaimSlot (void)
{
	if (m_nSlotsUsed < m_nSlots)
	{
		return m_nSlotsUsed++;			// still filling
	}

	// Sampled least-recently-used: look at a few slots picked at random and
	// drop the oldest of those. See the header for why this is not true LRU.
	u32 nVictim = Random (m_nSlots);
	u64 nOldest = m_pSlot[nVictim].nStamp;

	for (unsigned i = 1; i < DISKCACHE_LRU_SAMPLE; i++)
	{
		u32 nCandidate = Random (m_nSlots);
		if (m_pSlot[nCandidate].nStamp < nOldest)
		{
			nVictim = nCandidate;
			nOldest = m_pSlot[nCandidate].nStamp;
		}
	}

	IndexRemove (nVictim);
	m_pSlot[nVictim].nSector = DISKCACHE_NO_SECTOR;
	m_nEvictions++;

	return nVictim;
}

void CDiskCacheDevice::Admit (u64 nSector, const u8 *pData)
{
	u32 nSlot = ClaimSlot ();

	m_pSlot[nSlot].nSector = nSector;
	m_pSlot[nSlot].nStamp = m_nAccessCounter;
	memcpy (SlotData (nSlot), pData, DISKCACHE_SECTOR_SIZE);
	IndexInsert (nSlot);

	m_nAdmissions++;
}

void CDiskCacheDevice::Invalidate (u64 nSector)
{
	u32 nSlot = Lookup (nSector);
	if (nSlot == DISKCACHE_NO_SLOT)
	{
		return;
	}

	IndexRemove (nSlot);
	m_pSlot[nSlot].nSector = DISKCACHE_NO_SECTOR;

	// Left with the oldest possible stamp, so the next victim sample that
	// happens on it takes it in preference to anything holding real data.
	m_pSlot[nSlot].nStamp = 0;
}

// The memory of what has been asked for but is not being kept, which is what
// makes "admit on the second reference" possible without keeping the first.
// One direct-mapped array of sector tags: a collision forgets an old sighting,
// which costs an admission that could have happened, and never invents one.
//
// The tag is the low half of the sector number, so on any card small enough
// for its sector numbers to fit in 32 bits — anything under two terabytes —
// a match is exact.
boolean CDiskCacheDevice::HistorySeen (u64 nSector)
{
	if (m_pHistory == 0)
	{
		return FALSE;
	}

	unsigned nEntry = HashSector (nSector) & m_nHistoryMask;
	u32 nTag = (u32) nSector + 1;			// zero means empty

	if (m_pHistory[nEntry] == nTag)
	{
		// About to enter the pool proper, so the sighting is spent.
		m_pHistory[nEntry] = 0;
		return TRUE;
	}

	m_pHistory[nEntry] = nTag;
	return FALSE;
}

// ---------------------------------------------------------------------------
// CDevice
// ---------------------------------------------------------------------------

u64 CDiskCacheDevice::Seek (u64 ullOffset)
{
	if (m_pDevice == 0)
	{
		return (u64) -1;
	}

	u64 ullResult = m_pDevice->Seek (ullOffset);
	if (ullResult != (u64) -1)
	{
		m_ullOffset = ullOffset;
	}
	return ullResult;
}

u64 CDiskCacheDevice::GetSize (void) const
{
	if (m_pDevice == 0)
	{
		return (u64) -1;
	}
	return m_pDevice->GetSize ();
}

// Write-through means there is never anything to flush, but the real device
// may have its own reason to act on this, so it still goes through.
int CDiskCacheDevice::IOCtl (unsigned long ulCmd, void *pData)
{
	if (m_pDevice == 0)
	{
		return -1;
	}
	return m_pDevice->IOCtl (ulCmd, pData);
}

boolean CDiskCacheDevice::RemoveDevice (void)
{
	if (m_pDevice == 0)
	{
		return FALSE;
	}

	if (!m_pDevice->RemoveDevice ())
	{
		return FALSE;
	}

	// The real device is gone, so this object must stop answering to its
	// name: a wrapper around nothing would fail every read for ever, where an
	// absent name lets the layer above report the card as not ready. The pool
	// goes with it — every block in it describes a card that is no longer
	// there.
	CDeviceNameService *pNames = CDeviceNameService::Get ();
	if (pNames != 0)
	{
		pNames->RemoveDevice (m_pDeviceName, TRUE);
	}
	ReleasePool ();
	m_pDevice = 0;

	return TRUE;
}

int CDiskCacheDevice::Read (void *pBuffer, size_t nCount)
{
	if (m_pDevice == 0)
	{
		return -1;
	}

	u64 nStartSector = m_ullOffset / DISKCACHE_SECTOR_SIZE;
	u32 nSectors = (u32) (nCount / DISKCACHE_SECTOR_SIZE);
	boolean bWholeSectors =    (nCount % DISKCACHE_SECTOR_SIZE) == 0
				&& nSectors > 0
				&& (m_ullOffset % DISKCACHE_SECTOR_SIZE) == 0;

	// Recency is measured in sectors asked for rather than in requests, so
	// that the age of a block is an upper bound on how many distinct blocks
	// could have been touched since — which is what makes the age of a hit
	// mean something about pool size.
	m_nAccessCounter += nSectors;

	// Everything present? Then the card is not touched at all.
	if (m_nSlots > 0 && bWholeSectors)
	{
		boolean bAllResident = TRUE;
		for (u32 i = 0; i < nSectors; i++)
		{
			if (Lookup (nStartSector + i) == DISKCACHE_NO_SLOT)
			{
				bAllResident = FALSE;
				break;
			}
		}

		if (bAllResident)
		{
			u8 *pOut = (u8 *) pBuffer;
			u64 nOldestAge = 0;

			for (u32 i = 0; i < nSectors; i++)
			{
				u32 nSlot = Lookup (nStartSector + i);
				u64 nAge = m_nAccessCounter - m_pSlot[nSlot].nStamp;
				if (nAge > nOldestAge)
				{
					nOldestAge = nAge;
				}

				memcpy (pOut, SlotData (nSlot), DISKCACHE_SECTOR_SIZE);
				m_pSlot[nSlot].nStamp = m_nAccessCounter;
				pOut += DISKCACHE_SECTOR_SIZE;
			}

			m_nAgeBucket[AgeBucket (nOldestAge)]++;
			m_nCacheHits++;
			m_nHitSectors += nSectors;

			RecordRequest (&m_Read, nSectors);
			m_ullOffset += nCount;
			RecordSequence (nStartSector, nSectors);

			return (int) nCount;
		}
	}

	// Not all present. One transaction for the whole range, which is cheaper
	// than several even where part of it was already held: the card charges
	// mostly for being asked at all.
	unsigned nBefore = CTimer::GetClockTicks ();
	int nResult = m_pDevice->Read (pBuffer, nCount);
	unsigned nMicros = CTimer::GetClockTicks () - nBefore;

	if (nResult > 0)
	{
		m_ullOffset += (u64) nResult;
	}

	RecordRequest (&m_Read, nSectors);
	RecordTiming (&m_Read, nMicros, nResult >= 0);
	RecordSequence (nStartSector, nSectors);

	// Admission. A block that was already held is refreshed and made recent;
	// one seen for the second time enters the pool; one seen for the first
	// time is only remembered, so a file read once from end to end passes
	// through without displacing anything.
	if (m_nSlots > 0 && bWholeSectors && nResult == (int) nCount)
	{
		const u8 *pIn = (const u8 *) pBuffer;
		for (u32 i = 0; i < nSectors; i++, pIn += DISKCACHE_SECTOR_SIZE)
		{
			u64 nSector = nStartSector + i;
			u32 nSlot = Lookup (nSector);

			if (nSlot != DISKCACHE_NO_SLOT)
			{
				memcpy (SlotData (nSlot), pIn, DISKCACHE_SECTOR_SIZE);
				m_pSlot[nSlot].nStamp = m_nAccessCounter;
			}
			else if (HistorySeen (nSector))
			{
				Admit (nSector, pIn);
			}
			else
			{
				m_nFirstSightings++;
			}
		}
	}

	return nResult;
}

int CDiskCacheDevice::Write (const void *pBuffer, size_t nCount)
{
	if (m_pDevice == 0)
	{
		return -1;
	}

	// Kept because the transfer moves the position, and the range that has
	// to be reconciled with the pool afterwards is the one asked for.
	u64 ullStart = m_ullOffset;
	u64 nStartSector = ullStart / DISKCACHE_SECTOR_SIZE;
	u32 nSectors = (u32) (nCount / DISKCACHE_SECTOR_SIZE);
	boolean bWholeSectors =    (nCount % DISKCACHE_SECTOR_SIZE) == 0
				&& nSectors > 0
				&& (ullStart % DISKCACHE_SECTOR_SIZE) == 0;

	// Write-through, and the card gets it first. Nothing is ever held back:
	// this machine is switched off without warning by design.
	unsigned nBefore = CTimer::GetClockTicks ();
	int nResult = m_pDevice->Write (pBuffer, nCount);
	unsigned nMicros = CTimer::GetClockTicks () - nBefore;

	if (nResult > 0)
	{
		m_ullOffset += (u64) nResult;
	}

	RecordRequest (&m_Write, nSectors);
	RecordTiming (&m_Write, nMicros, nResult >= 0);

	if (m_nSlots > 0)
	{
		if (bWholeSectors && nResult == (int) nCount)
		{
			// Refresh what is held so the pool still agrees with the
			// card. A write does not admit anything: writing to a block
			// is not evidence that it will be read.
			const u8 *pIn = (const u8 *) pBuffer;
			for (u32 i = 0; i < nSectors; i++, pIn += DISKCACHE_SECTOR_SIZE)
			{
				u32 nSlot = Lookup (nStartSector + i);
				if (nSlot != DISKCACHE_NO_SLOT)
				{
					memcpy (SlotData (nSlot), pIn,
						DISKCACHE_SECTOR_SIZE);
					m_pSlot[nSlot].nStamp = m_nAccessCounter;
					m_nWriteUpdates++;
				}
			}
		}
		else
		{
			// A partial or failed write, or one that does not land on
			// sector boundaries, leaves this layer unable to say what
			// the card now holds — so it stops claiming to know, over
			// every sector the request touched even partly.
			u64 nFirst = ullStart / DISKCACHE_SECTOR_SIZE;
			u64 nLast = (ullStart + nCount + DISKCACHE_SECTOR_SIZE - 1)
				    / DISKCACHE_SECTOR_SIZE;
			for (u64 s = nFirst; s < nLast; s++)
			{
				Invalidate (s);
			}
		}
	}

	// A write breaks the read-ahead prediction: the next read is not a
	// continuation of anything this layer has seen.
	m_bHavePrevious = FALSE;

	return nResult;
}

// ---------------------------------------------------------------------------
// Counting
// ---------------------------------------------------------------------------

unsigned CDiskCacheDevice::HighestBit (u64 nValue)
{
	unsigned nBit = 0;
	while (nValue > 1)
	{
		nValue >>= 1;
		nBit++;
	}
	return nBit;
}

unsigned CDiskCacheDevice::SizeBucket (u32 nSectors)
{
	if (nSectors == 0)
	{
		return 0;
	}

	unsigned nBucket = HighestBit (nSectors);
	if (nBucket >= DISKCACHE_SIZE_BUCKETS)
	{
		nBucket = DISKCACHE_SIZE_BUCKETS - 1;
	}
	return nBucket;
}

unsigned CDiskCacheDevice::JumpBucket (u64 nDistanceSectors)
{
	if (nDistanceSectors < 8)	return 0;
	if (nDistanceSectors < 64)	return 1;
	if (nDistanceSectors < 512)	return 2;
	if (nDistanceSectors < 4096)	return 3;
	if (nDistanceSectors < 32768)	return 4;
	return 5;
}

// How long a hit block had gone untouched, in blocks, turned into the smallest
// pool that would still have been holding it. A block last used N accesses ago
// has had at most N distinct blocks touched since, so a pool of N blocks under
// least-recently-used would have kept it. That makes the resulting curve a
// floor: the real hit rate at a given size is this or better, never worse.
unsigned CDiskCacheDevice::AgeBucket (u64 nAgeBlocks)
{
	u64 nUnits = nAgeBlocks / DISKCACHE_AGE_FIRST_BLOCKS;
	if (nUnits == 0)
	{
		return 0;
	}

	unsigned nBucket = HighestBit (nUnits) + 1;
	if (nBucket >= DISKCACHE_AGE_BUCKETS)
	{
		nBucket = DISKCACHE_AGE_BUCKETS - 1;
	}
	return nBucket;
}

void CDiskCacheDevice::RecordRequest (TDirStats *pDir, u32 nSectors)
{
	pDir->nRequests++;
	pDir->nSectors += nSectors;
	pDir->nSizeBucket[SizeBucket (nSectors)]++;
}

void CDiskCacheDevice::RecordTiming (TDirStats *pDir, unsigned nMicros, boolean bOK)
{
	if (!bOK)
	{
		pDir->nFailures++;
		return;		// a failed transfer's duration says nothing useful
	}

	pDir->nTimedRequests++;
	pDir->nTotalMicros += nMicros;
	if (nMicros < pDir->nMinMicros)
	{
		pDir->nMinMicros = nMicros;
	}
	if (nMicros > pDir->nMaxMicros)
	{
		pDir->nMaxMicros = nMicros;
	}
}

void CDiskCacheDevice::RecordSequence (u64 nStartSector, u32 nSectors)
{
	if (m_bHavePrevious)
	{
		if (nStartSector == m_nNextExpected)
		{
			m_nSequential++;
		}
		else if (nStartSector > m_nNextExpected)
		{
			m_nJumpForward++;
			m_nJumpBucket[JumpBucket (nStartSector - m_nNextExpected)]++;
		}
		else
		{
			m_nJumpBackward++;
			m_nJumpBucket[JumpBucket (m_nNextExpected - nStartSector)]++;
		}
	}

	m_nNextExpected = nStartSector + nSectors;
	m_bHavePrevious = TRUE;
}

// ---------------------------------------------------------------------------
// The report
// ---------------------------------------------------------------------------

void CDiskCacheDevice::Poll (void)
{
	if (!m_bStarted)
	{
		m_ullFirstTicks = CTimer::GetClockTicks64 ();
		m_ullLastReportTicks = m_ullFirstTicks;
		m_bStarted = TRUE;
		return;
	}

	u64 ullNow = CTimer::GetClockTicks64 ();
	if (ullNow - m_ullLastReportTicks < (u64) DISKCACHE_REPORT_SECONDS * CLOCKHZ)
	{
		return;
	}

	Report ();
}

void CDiskCacheDevice::ReportLegend (void)
{
	CLogger *pLog = CLogger::Get ();

	pLog->Write (From, LogNotice,
		     "what the card was asked for, and what this cache did with it. Writes are "
		     "never held back: the card has every one of them before the call returns");
	pLog->Write (From, LogNotice,
		     "one sector is %u bytes, and every size and distance below is in sectors",
		     (unsigned) DISKCACHE_SECTOR_SIZE);
	pLog->Write (From, LogNotice,
		     "sizes  = how many requests were 1 sector long, how many were 2-3, and so on");
	pLog->Write (From, LogNotice,
		     "seq    = reads beginning exactly where the previous read ended; jump = the "
		     "rest, and how far from that point they began");
	pLog->Write (From, LogNotice,
		     "cache  = the pool, and the most of it ever in use. If that high-water mark "
		     "stays well under the pool, a bigger pool cannot help this program");
	pLog->Write (From, LogNotice,
		     "hits   = requests answered entirely from memory, with no card transaction "
		     "at all");
	pLog->Write (From, LogNotice,
		     "admit  = a block enters the pool only when asked for a second time, so a "
		     "file read once from end to end never displaces one being read in a loop");
	pLog->Write (From, LogNotice,
		     "curve  = the share of reads a smaller pool would still have caught, judged "
		     "by how long each hit block had gone untouched. A floor, not an exact "
		     "figure: the real rate at a size is this or better");
	pLog->Write (From, LogNotice,
		     "card   = time inside the card driver per request, and the time the cache "
		     "meant nobody had to spend");
}

void CDiskCacheDevice::Report (void)
{
	CLogger *pLog = CLogger::Get ();
	if (pLog == 0)
	{
		return;
	}

	static const char *AgeLabel[DISKCACHE_AGE_BUCKETS] =
	{
		"512K", "1M", "2M", "4M", "8M", "16M", "32M", ">32M"
	};

	u64 ullNow = CTimer::GetClockTicks64 ();
	unsigned nIntervalSeconds =
		(unsigned) ((ullNow - m_ullLastReportTicks + CLOCKHZ / 2) / CLOCKHZ);
	unsigned nUptimeSeconds = (unsigned) ((ullNow - m_ullFirstTicks) / CLOCKHZ);

	m_ullLastReportTicks = ullNow;

	u64 nNewReads = m_Read.nRequests - m_nPrevReadRequests;
	u64 nNewReadSectors = m_Read.nSectors - m_nPrevReadSectors;
	u64 nNewWrites = m_Write.nRequests - m_nPrevWriteRequests;
	u64 nNewWriteSectors = m_Write.nSectors - m_nPrevWriteSectors;

	m_nPrevReadRequests = m_Read.nRequests;
	m_nPrevReadSectors = m_Read.nSectors;
	m_nPrevWriteRequests = m_Write.nRequests;
	m_nPrevWriteSectors = m_Write.nSectors;

	// A quiet interval gets one line. There is nothing new to describe, and
	// the report is polled serial output: printing the whole thing at an idle
	// card would cost the machine more than the information is worth.
	if (nNewReads == 0 && nNewWrites == 0)
	{
		pLog->Write (From, LogNotice,
			     "t+%us  idle, no disk traffic  (so far: %llu reads / %llu KB, "
			     "%llu writes / %llu KB)",
			     nUptimeSeconds,
			     (unsigned long long) m_Read.nRequests,
			     (unsigned long long) (m_Read.nSectors * DISKCACHE_SECTOR_SIZE / 1024),
			     (unsigned long long) m_Write.nRequests,
			     (unsigned long long) (m_Write.nSectors * DISKCACHE_SECTOR_SIZE / 1024));
		return;
	}

	if (!m_bLegendPrinted)
	{
		ReportLegend ();
		m_bLegendPrinted = TRUE;
	}

	// Reads. Everything is a running total since boot except the bracket,
	// which is this interval alone — the totals size a pool, the bracket says
	// what the program is doing right now.
	unsigned nReadRateKB = 0;
	if (nIntervalSeconds > 0)
	{
		nReadRateKB = (unsigned) (nNewReadSectors * DISKCACHE_SECTOR_SIZE
					  / 1024 / nIntervalSeconds);
	}
	unsigned nReadMeanTenths = MeanTenths (m_Read.nSectors, m_Read.nRequests);

	pLog->Write (From, LogNotice,
		     "t+%us  reads %llu / %llu KB, mean %u.%u sectors  "
		     "(last %us: %llu reqs, %llu KB, %u KB/s)",
		     nUptimeSeconds,
		     (unsigned long long) m_Read.nRequests,
		     (unsigned long long) (m_Read.nSectors * DISKCACHE_SECTOR_SIZE / 1024),
		     nReadMeanTenths / 10, nReadMeanTenths % 10,
		     nIntervalSeconds,
		     (unsigned long long) nNewReads,
		     (unsigned long long) (nNewReadSectors * DISKCACHE_SECTOR_SIZE / 1024),
		     nReadRateKB);

	pLog->Write (From, LogNotice,
		     "  sizes  1:%llu  2-3:%llu  4-7:%llu  8-15:%llu  16-31:%llu  "
		     "32-63:%llu  64-127:%llu  128+:%llu",
		     (unsigned long long) m_Read.nSizeBucket[0],
		     (unsigned long long) m_Read.nSizeBucket[1],
		     (unsigned long long) m_Read.nSizeBucket[2],
		     (unsigned long long) m_Read.nSizeBucket[3],
		     (unsigned long long) m_Read.nSizeBucket[4],
		     (unsigned long long) m_Read.nSizeBucket[5],
		     (unsigned long long) m_Read.nSizeBucket[6],
		     (unsigned long long) m_Read.nSizeBucket[7]);

	// Sequentiality. The denominator is every read that had a predecessor to
	// be judged against, so the very first read of a run counts for neither.
	u64 nJudged = m_nSequential + m_nJumpForward + m_nJumpBackward;
	u64 nJumped = m_nJumpForward + m_nJumpBackward;
	unsigned nSeqTenths = PercentTenths (m_nSequential, nJudged);
	pLog->Write (From, LogNotice,
		     "  seq    %llu of %llu (%u.%u%%)   jump %llu (%llu on, %llu back)  "
		     "<8:%llu <64:%llu <512:%llu <4K:%llu <32K:%llu 32K+:%llu",
		     (unsigned long long) m_nSequential,
		     (unsigned long long) nJudged,
		     nSeqTenths / 10, nSeqTenths % 10,
		     (unsigned long long) nJumped,
		     (unsigned long long) m_nJumpForward,
		     (unsigned long long) m_nJumpBackward,
		     (unsigned long long) m_nJumpBucket[0],
		     (unsigned long long) m_nJumpBucket[1],
		     (unsigned long long) m_nJumpBucket[2],
		     (unsigned long long) m_nJumpBucket[3],
		     (unsigned long long) m_nJumpBucket[4],
		     (unsigned long long) m_nJumpBucket[5]);

	if (m_nSlots == 0)
	{
		pLog->Write (From, LogNotice,
			     "  cache  off — every one of those reads reached the card");
	}
	else
	{
		// Occupancy is the number that says whether a bigger pool could
		// possibly help. A high-water mark short of the pool means the
		// program never had more worth keeping than it already had room
		// for, and every larger size will report the same hit rate.
		unsigned nUsedKB = (unsigned) ((u64) m_nSlotsUsed
					       * DISKCACHE_SECTOR_SIZE / 1024);
		unsigned nUsedTenths = PercentTenths (m_nSlotsUsed, m_nSlots);
		pLog->Write (From, LogNotice,
			     "  cache  %u KB pool in %u blocks (+%u KB bookkeeping), "
			     "high-water %u KB used (%u.%u%%)",
			     m_nPoolKB, m_nSlots, m_nOverheadKB,
			     nUsedKB, nUsedTenths / 10, nUsedTenths % 10);

		unsigned nHitTenths = PercentTenths (m_nCacheHits, m_Read.nRequests);
		pLog->Write (From, LogNotice,
			     "  hits   %llu of %llu (%u.%u%%), %llu KB never asked of the card",
			     (unsigned long long) m_nCacheHits,
			     (unsigned long long) m_Read.nRequests,
			     nHitTenths / 10, nHitTenths % 10,
			     (unsigned long long) (m_nHitSectors * DISKCACHE_SECTOR_SIZE / 1024));

		pLog->Write (From, LogNotice,
			     "  admit  %llu admitted on a second sighting, %llu evicted, "
			     "%llu seen once and not kept, %llu refreshed by a write",
			     (unsigned long long) m_nAdmissions,
			     (unsigned long long) m_nEvictions,
			     (unsigned long long) m_nFirstSightings,
			     (unsigned long long) m_nWriteUpdates);

		// Cumulative: each figure is what a pool of that size or larger
		// would have caught, so the row only ever climbs.
		CString Curve;
		u64 nRunning = 0;
		for (unsigned i = 0; i < DISKCACHE_AGE_BUCKETS; i++)
		{
			nRunning += m_nAgeBucket[i];
			unsigned nTenths = PercentTenths (nRunning, m_Read.nRequests);
			CString Part;
			Part.Format (" %s:%u.%u%%", AgeLabel[i],
				     nTenths / 10, nTenths % 10);
			Curve.Append (Part);
		}
		pLog->Write (From, LogNotice, "  curve %s", (const char *) Curve);
	}

	if (m_Read.nTimedRequests > 0)
	{
		// What the cache is worth, in the only currency that matters here:
		// time the machine did not spend waiting for the card, priced at
		// what a read from it actually costs on this board.
		u64 nMeanMicros = m_Read.nTotalMicros / m_Read.nTimedRequests;
		pLog->Write (From, LogNotice,
			     "  card   %llu reads reached it: min %uus, mean %lluus, max %uus; "
			     "%llu ms spent, about %llu ms not spent",
			     (unsigned long long) m_Read.nTimedRequests,
			     m_Read.nMinMicros,
			     (unsigned long long) nMeanMicros,
			     m_Read.nMaxMicros,
			     (unsigned long long) (m_Read.nTotalMicros / 1000),
			     (unsigned long long) (m_nCacheHits * nMeanMicros / 1000));
	}

	// Writes get one line: they are rare here, and they always reach the
	// card, so there is no cache outcome to describe.
	if (m_Write.nRequests > 0)
	{
		unsigned nWriteMeanTenths = MeanTenths (m_Write.nSectors, m_Write.nRequests);
		pLog->Write (From, LogNotice,
			     "  writes %llu / %llu KB, mean %u.%u sectors, %llu ms in the "
			     "driver  (last %us: %llu reqs, %llu KB)",
			     (unsigned long long) m_Write.nRequests,
			     (unsigned long long) (m_Write.nSectors * DISKCACHE_SECTOR_SIZE / 1024),
			     nWriteMeanTenths / 10, nWriteMeanTenths % 10,
			     (unsigned long long) (m_Write.nTotalMicros / 1000),
			     nIntervalSeconds,
			     (unsigned long long) nNewWrites,
			     (unsigned long long) (nNewWriteSectors * DISKCACHE_SECTOR_SIZE / 1024));
	}

	if (m_Read.nFailures != 0 || m_Write.nFailures != 0)
	{
		pLog->Write (From, LogError, "  %llu reads and %llu writes FAILED",
			     (unsigned long long) m_Read.nFailures,
			     (unsigned long long) m_Write.nFailures);
	}
}
