//
// diskcache.h — a read cache that sits in front of a Circle block device, and
// the instrument that measures whether it is earning its memory.
//
// WHAT IT IS. A CDevice that wraps another CDevice — in practice the SD card —
// and answers reads from a pool of memory when it already holds the sectors
// asked for. Everything it cannot answer is handed to the real device
// unchanged and its result returned untouched. It also counts every request
// that goes past, in both states, and prints what it has seen every few
// seconds, because the right pool size for a given program is a measured
// number and not a guess.
//
// WRITES ARE ALWAYS WRITE-THROUGH. Every write reaches the card before this
// class returns, and a cached copy of a written sector is updated to match. No
// dirty blocks exist at any moment, behind any setting. This is not a
// performance trade-off that was lost, it is the only safe design for the
// machine: there is no clean shutdown here and no operating system to ask for
// one. The board is switched off mid-frame, on purpose, and anything held back
// in memory at that instant is simply gone.
//
// HOW IT GETS IN THE WAY. Circle's FatFs glue does not hold a pointer to the
// SD card. It looks the card up by name through CDeviceNameService — "emmc1"
// for the internal slot — and then calls Seek and Read on whatever it was
// handed. So a CDevice registered under that name is used by everything above
// it with nothing above it changed: FatFs, newlib's fopen and fread, and any
// library doing its own file access all arrive here without knowing it. The
// takeover is Install(), which looks the real device up, unregisters the name
// and registers this object in its place. The real device object is untouched
// and is not destroyed; only the name now resolves somewhere else.
//
// ONE THING DOES NOT PASS THROUGH. A caller that asks to be told when the
// device disappears — CDevice::RegisterRemovedHandler — registers with this
// object, and CDevice keeps that list privately with no way to fire it from a
// subclass, so the notification cannot be relayed from the real device. What
// this class does instead is give up the name when a removal it was asked for
// succeeds, so the layer above stops finding a device at all rather than
// finding one that fails every read. On an internal SD card the question never
// comes up: it cannot be removed and Circle's EMMC driver refuses the request.
// A host putting this in front of a device that really can be unplugged should
// settle that before relying on it.
//
// THE THREE DECISIONS INSIDE IT, and why each is the way it is:
//
//   ADMISSION — a block enters the pool only when it is asked for a SECOND
//   time. One rule separates the two patterns that matter without the program
//   having to describe itself: a looping sound track admits itself on its
//   second pass round the loop and then stays, while a level file or a voice
//   package streamed once from end to end is never admitted at all and so
//   never pushes out anything useful. Deciding that needs a memory of what has
//   been asked for recently but is not being kept, which is what the history
//   filter below is: sector numbers only, no data, one small array.
//
//   EVICTION — a few slots are picked at random and the least recently used
//   of THAT SAMPLE is dropped. Not true least-recently-used, and the
//   difference is the whole point. Cyclic access is everywhere in this kind of
//   program, and under true LRU a loop very slightly larger than the pool
//   returns no hits at all: every block is thrown out exactly one access
//   before it is wanted again. That failure is not a gentle slope, it is a
//   cliff, and from the outside it looks like the cache doing nothing when the
//   real cause is a track being a few percent too long. Choosing the victim at
//   random breaks the lockstep. Keeping recency inside the sample means a
//   block that survives one pass is used again, becomes recent, and is likely
//   to survive the next — so survivors accumulate into a stable resident set
//   instead of the pool re-shuffling for ever. It is also cheaper than true
//   LRU: a counter written into a slot and a handful of comparisons, with no
//   list to thread through every hit and no pointers to corrupt.
//
//   LOOKUP AND EVICTION ARE BOTH FIXED-COST. A chained hash index finds a
//   sector without scanning the pool, and the victim sample never walks it
//   either. This is what makes the pool size free of processor cost, which is
//   the entire argument for making it large.
//
// READ-AHEAD, which is a separate thing from the pool and answers a different
// problem. A pool only helps data that is asked for twice. Plenty of real
// reading is a file streamed from beginning to end and never touched again,
// one sector at a time, and no cache of any size can help that: nothing is
// ever asked for twice. What can help is that the card charges almost the
// same for a large request as a small one, so the cost of such a stream is
// the NUMBER of requests and not the amount of data.
//
// So when a read continues exactly where the previous one ended, this class
// asks the card for a whole run of sectors in one transaction and keeps it in
// a small window. The reads that follow are answered from that window without
// the card being asked at all.
//
// THE WINDOW IS NOT THE POOL, and read-ahead never puts anything in the pool.
// The window holds one run, it is thrown away the moment the next one
// replaces it, and a sector served from it goes through exactly the same
// admission test as a sector served from the card. So a file streamed once
// still never enters the pool and still cannot displace anything, while a
// file streamed round a loop is seen a second time and admitted then, as any
// other repeat would be. Read-ahead makes the stream cheap; it does not make
// it privileged.
//
// Depth is a real trade-off in both directions, which is why it is set at
// runtime beside the pool size rather than chosen here. Too shallow and the
// per-request toll is still being paid. Too deep and the card is being asked
// for sectors nobody wants, which costs time of its own. The report prints
// how much of each window was actually used, so the depth can be judged
// rather than guessed at.
//
// MEMORY. One allocation at Configure() time, from the LOW heap explicitly,
// and none ever again — the read path allocates nothing at all. Low heap is
// deliberate: on a board with more than a gigabyte the excess is a separate
// heap that this project does not use yet, and staying low keeps one identical
// path on every board.
//
// WHERE IT MAY RUN. Circle's devices belong to core 0, so every call that
// reaches this class arrives on core 0 already — that is a property of the
// host, not of this class. It therefore logs with CLogger directly, which is
// legal only because of that. If a host ever routes disk access from another
// core, this class must be revisited before anything else is.
//
// PORTABILITY. Circle and nothing else: no SDL, no host-specific types, one
// header and one source file. Nothing in it knows which program it is serving.
//
#ifndef _diskcache_h
#define _diskcache_h

#include <circle/device.h>
#include <circle/string.h>
#include <circle/types.h>

// Requests are counted in sectors, and this is the size of one. Every layer in
// this stack — the SD card, FatFs and Circle's own glue — uses 512 bytes and
// none of them offers a way to ask. It is also the cache's block size, which
// is the right choice while nearly every read is a single sector.
#define DISKCACHE_SECTOR_SIZE       512

// The pool size used when the host names none, in kilobytes.
//
// Chosen to be larger than any program has been measured to want, rather than
// to fit any of them. What a program actually occupies varies enormously — a
// few kilobytes where nothing is re-read, a few megabytes for the heaviest
// reader seen — so a shared number cannot be economical for all of them and
// should not try. It can be generous instead: on the boards this runs on, this
// much memory is not worth reclaiming, and being larger than the working set is
// what makes the pool stop being the limit.
//
// A program that wants its own figure names it at boot. The report is how to
// find it: the high-water occupancy says what was really used, and a program
// whose occupancy sits far below the pool has its answer.
#define DISKCACHE_DEFAULT_KB        8192

// The read-ahead window used when the host names none, in kilobytes. Zero
// turns read-ahead off.
//
// This trades between two things that do not peak together, and it is worth
// knowing which way it errs. Total time in the card driver keeps falling as
// the window grows, well past this depth. The worst SINGLE read grows too, and
// much faster: a deep window is one transaction the calling core sits blocked
// on, so read-ahead trades many small waits for one large one. Totals call
// that a pure win; a program drawing frames does not, because the same delay
// spread over many reads is invisible and gathered into one read lands inside
// a frame and drops it.
//
// This is set well below the fastest depth on purpose, at the point where the
// worst single read stays a small fraction of a frame — so a program pays no
// visible price for it even when it reads while drawing. It still takes the
// large majority of the reduction in total time, which is what makes this
// depth worth having on by default rather than something to opt into.
//
// A program that does all its reading while nothing is drawn — loading
// screens, and most games — can go several times deeper for a further gain,
// and the switch is there for that. The report gives both numbers it trades
// between: the share of each window that was actually wanted, and the largest
// single read.
#define DISKCACHE_DEFAULT_READAHEAD_KB  4

// A window deeper than this is refused however it was asked for. Beyond it a
// single transaction takes long enough that the card stops being the thing
// worth optimising.
#define DISKCACHE_MAX_READAHEAD_KB  1024

// The window is also held to this fraction of the pool. The two are separate
// allocations and a deep window cannot evict a pool block directly, but a
// window that dwarfs the pool it sits beside is a sign the sizes were not
// meant, and it costs memory that the pool would have used better.
#define DISKCACHE_READAHEAD_POOL_DIVISOR    4

// How many slots are examined when a victim is chosen. One would be pure
// random and would throw away recency entirely; every slot would be true LRU
// and would phase-lock against cyclic reads. A handful keeps most of the
// benefit of recency while leaving enough noise to break a cycle.
#define DISKCACHE_LRU_SAMPLE        5

// How often a report reaches the log, in seconds.
#define DISKCACHE_REPORT_SECONDS    5

// Request sizes and seek distances are each counted into a small set of
// power-of-two buckets rather than kept individually.
#define DISKCACHE_SIZE_BUCKETS      8
#define DISKCACHE_JUMP_BUCKETS      6

// Hits are also counted by how long the block had gone untouched, which is
// what produces a hit-rate curve against pool size. The first bucket is half a
// megabyte and each one after it doubles.
#define DISKCACHE_AGE_BUCKETS       8
#define DISKCACHE_AGE_FIRST_BLOCKS  1024	// 512 KB, in blocks

class CDiskCacheDevice : public CDevice
{
public:
	/// \param pDeviceName Name in the device name service to take over.
	///        "emmc1" is the internal SD slot, which is what FatFs mounts.
	CDiskCacheDevice (const char *pDeviceName = "emmc1");
	~CDiskCacheDevice (void);

	/// \brief Take the name over from the real device.
	/// Call after the real device has been initialised — it is not in the
	/// name service before that — and before anything mounts or opens it.
	/// The cache is not yet holding anything at this point: Install() only
	/// puts this object in the path, so the host can decide the size later,
	/// once it has read whatever tells it the size.
	/// \return TRUE if the name was found and now resolves to this object.
	boolean Install (void);

	/// \brief Give the cache its memory. Call once, before the program runs.
	/// \param nKilobytes Pool size. Zero means no pool at all: no read is
	///        ever answered from held data, and only the counting stays on.
	/// \param nReadAheadKB Read-ahead window. Zero means the card is asked
	///        for exactly what was requested and nothing more. Held to a
	///        fraction of the pool where there is one, so a window can never
	///        be set out of all proportion to what it feeds.
	/// \return TRUE if the memory asked for was obtained, or if nothing was
	///         asked for. FALSE means it was refused and that part is off.
	/// \note Both are zero for the run every other setting is compared
	///       against: the card, unassisted, with the counting still running.
	boolean Configure (unsigned nKilobytes, unsigned nReadAheadKB);

	/// \brief Emit a report if the reporting interval has elapsed.
	/// Costs one clock read when it is not yet time, so it can be called as
	/// often as the host likes: an idle loop is the natural home. It must be
	/// called from somewhere allowed to write to the log — core 0, and not
	/// from inside a call another core is waiting on.
	void Poll (void);

	/// \brief Emit a report now, whether or not the interval has elapsed.
	void Report (void);

	// --- CDevice ---
	int Read (void *pBuffer, size_t nCount) override;
	int Write (const void *pBuffer, size_t nCount) override;
	u64 Seek (u64 ullOffset) override;
	u64 GetSize (void) const override;
	int IOCtl (unsigned long ulCmd, void *pData) override;
	boolean RemoveDevice (void) override;

private:
	// One slot of the pool. The bytes themselves live in a separate flat
	// array, so this stays small and a sample of several slots touches few
	// cache lines.
	struct TSlot
	{
		u64 nSector;		// resident sector, or DISKCACHE_NO_SECTOR
		u64 nStamp;		// access counter when last used
		u32 nHashNext;		// next slot in the same bucket
		u32 nReserved;
	};

	// One direction's worth of counters. Reads and writes are counted the
	// same way, so they share a shape.
	struct TDirStats
	{
		u64 nRequests;		// everything asked of this device
		u64 nSectors;
		u64 nTimedRequests;	// the subset that reached the real device
		u64 nFailures;
		u64 nTotalMicros;
		unsigned nMinMicros;
		unsigned nMaxMicros;
		u64 nSizeBucket[DISKCACHE_SIZE_BUCKETS];
	};

	void ReleasePool (void);
	boolean ConfigurePool (unsigned nKilobytes);
	boolean ConfigureReadAhead (unsigned nReadAheadKB);

	// The read-ahead window. Holds one run of sectors, is replaced whole,
	// and never hands anything to the pool without admission agreeing.
	boolean ServeFromAhead (void *pBuffer, u64 nStartSector, u32 nSectors);
	int FillAhead (u64 nStartSector, u32 nSectors, unsigned *pMicros);
	void DropAhead (void);

	// Offer each sector of a range to the pool, on the terms every other
	// read gets: already held means refresh it, seen before means admit it,
	// never seen means remember it and nothing more.
	void OfferRange (u64 nStartSector, u32 nSectors, const u8 *pData);

	// The pool.
	u32 Lookup (u64 nSector) const;			// slot index, or NO_SLOT
	void IndexInsert (u32 nSlot);
	void IndexRemove (u32 nSlot);
	u32 ClaimSlot (void);				// free slot, or a victim
	void Admit (u64 nSector, const u8 *pData);
	void Invalidate (u64 nSector);
	u8 *SlotData (u32 nSlot) const;

	// The memory of what was asked for but not kept. Returns TRUE if this
	// sector has been seen before, and forgets it when it says so, because
	// the caller is about to put it in the pool proper.
	boolean HistorySeen (u64 nSector);

	u32 Random (unsigned nLimit);

	// Counting. Every request is recorded; only the ones that actually
	// reached the real device are timed, so the mean below is the card's
	// and not an average diluted by the ones it never saw.
	void ResetDir (TDirStats *pDir);
	void RecordRequest (TDirStats *pDir, u32 nSectors);
	void RecordTiming (TDirStats *pDir, unsigned nMicros, boolean bOK);
	void RecordSequence (u64 nStartSector, u32 nSectors);
	void ReportLegend (void);

	static unsigned SizeBucket (u32 nSectors);
	static unsigned JumpBucket (u64 nDistanceSectors);
	static unsigned AgeBucket (u64 nAgeBlocks);
	static unsigned HighestBit (u64 nValue);
	static unsigned NextPowerOfTwo (unsigned nValue);

	const char *m_pDeviceName;
	CDevice *m_pDevice;		// the real card, or 0 before Install()

	// Where the next Read or Write will land. Mirrors the real device's own
	// position: set by Seek, advanced by a successful transfer.
	u64 m_ullOffset;

	// The pool, all of it from one low-heap allocation each.
	u8 *m_pData;
	TSlot *m_pSlot;
	u32 *m_pBucket;
	u32 *m_pHistory;
	unsigned m_nSlots;
	unsigned m_nSlotsUsed;		// how many have ever been filled
	unsigned m_nBucketMask;
	unsigned m_nHistoryMask;
	unsigned m_nPoolKB;		// what was asked for
	unsigned m_nOverheadKB;		// what the bookkeeping costs on top

	// The read-ahead window: one run of sectors, thrown away whole.
	u8 *m_pAhead;
	unsigned m_nAheadDepth;		// sectors fetched per run, 0 = off
	u64 m_nAheadBase;		// first sector currently held
	u32 m_nAheadHeld;		// how many, 0 when the window is empty

	// Where the card ends, so a run is never asked for past it. Read once,
	// when the real device is adopted.
	u64 m_nDeviceSectors;

	// Recency, and the random source that keeps it from locking on to a
	// cyclic read pattern.
	u64 m_nAccessCounter;		// advances by one per sector requested
	u64 m_nRandomState;

	// Cache outcome.
	u64 m_nCacheHits;		// requests served entirely from memory
	u64 m_nHitSectors;
	u64 m_nAdmissions;
	u64 m_nEvictions;
	u64 m_nFirstSightings;		// seen once, remembered, not kept
	u64 m_nWriteUpdates;		// cached blocks a write refreshed
	u64 m_nAgeBucket[DISKCACHE_AGE_BUCKETS];

	// Read-ahead outcome. Fetched against used is the whole judgement on
	// the depth: sectors fetched that nobody went on to ask for were time
	// spent for nothing.
	u64 m_nAheadHits;		// requests answered from the window
	u64 m_nAheadHitSectors;
	u64 m_nAheadFetches;		// runs asked of the card
	u64 m_nAheadFetched;		// sectors those runs brought back
	u64 m_nAheadUsed;		// sectors of them actually handed out

	TDirStats m_Read;
	TDirStats m_Write;

	// Sequentiality. m_nNextExpected is the sector just past the end of the
	// last read, which is what a read-ahead would have fetched.
	u64 m_nNextExpected;
	boolean m_bHavePrevious;
	u64 m_nSequential;
	u64 m_nJumpForward;
	u64 m_nJumpBackward;
	u64 m_nJumpBucket[DISKCACHE_JUMP_BUCKETS];

	// Reporting. The wall clock is the 64-bit one: the 32-bit microsecond
	// counter wraps after about 71 minutes, which is well inside a session.
	// Individual transfers are timed with the 32-bit counter, where the wrap
	// cannot matter.
	u64 m_ullLastReportTicks;
	u64 m_ullFirstTicks;
	boolean m_bStarted;
	boolean m_bLegendPrinted;
	u64 m_nPrevReadRequests;
	u64 m_nPrevReadSectors;
	u64 m_nPrevWriteRequests;
	u64 m_nPrevWriteSectors;
};

#endif
