/*-------------------------------------------------------------------------
 *
 * clog.c
 *		PostgreSQL transaction-commit-log manager
 *
 * This module replaces the old "pg_log" access code, which treated pg_log
 * essentially like a relation, in that it went through the regular buffer
 * manager.  The problem with that was that there wasn't any good way to
 * recycle storage space for transactions so old that they'll never be
 * looked up again.  Now we use specialized access code so that the commit
 * log can be broken into relatively small, independent segments.
 *
 * XLOG interactions: this module generates an XLOG record whenever a new
 * CLOG page is initialized to zeroes.	Other writes of CLOG come from
 * recording of transaction commit or abort in xact.c, which generates its
 * own XLOG records for these events and will re-perform the status update
 * on redo; so we need make no additional XLOG entry here.	For synchronous
 * transaction commits, the XLOG is guaranteed flushed through the XLOG commit
 * record before we are called to log a commit, so the WAL rule "write xlog
 * before data" is satisfied automatically.  However, for async commits we
 * must track the latest LSN affecting each CLOG page, so that we can flush
 * XLOG that far and satisfy the WAL rule.	We don't have to worry about this
 * for aborts (whether sync or async), since the post-crash assumption would
 * be that such transactions failed anyway.
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/backend/access/transam/clog.c,v 1.47 2008/08/01 13:16:08 alvherre Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/clog.h"
#include "access/slru.h"
#include "access/transam.h"
#include "pg_trace.h"
#include "postmaster/bgwriter.h"
#include "cdb/cdbpersistentstore.h"

/*
 * Defines for CLOG page sizes.  A page is the same BLCKSZ as is used
 * everywhere else in Postgres.
 *
 * Note: because TransactionIds are 32 bits and wrap around at 0xFFFFFFFF,
 * CLOG page numbering also wraps around at 0xFFFFFFFF/CLOG_XACTS_PER_PAGE,
 * and CLOG segment numbering at 0xFFFFFFFF/CLOG_XACTS_PER_SEGMENT.  We need
 * take no explicit notice of that fact in this module, except when comparing
 * segment and page numbers in TruncateCLOG (see CLOGPagePrecedes).
 */

/* We need two bits per xact, so four xacts fit in a byte */
#define CLOG_BITS_PER_XACT	2
#define CLOG_XACTS_PER_BYTE 4
#define CLOG_XACTS_PER_PAGE (BLCKSZ * CLOG_XACTS_PER_BYTE)
#define CLOG_XACT_BITMASK	((1 << CLOG_BITS_PER_XACT) - 1)

#define TransactionIdToPage(xid)	((xid) / (TransactionId) CLOG_XACTS_PER_PAGE)
#define TransactionIdToPgIndex(xid) ((xid) % (TransactionId) CLOG_XACTS_PER_PAGE)
#define TransactionIdToByte(xid)	(TransactionIdToPgIndex(xid) / CLOG_XACTS_PER_BYTE)
#define TransactionIdToBIndex(xid)	((xid) % (TransactionId) CLOG_XACTS_PER_BYTE)

/* We store the latest async LSN for each group of transactions */
#define CLOG_XACTS_PER_LSN_GROUP	32	/* keep this a power of 2 */
#define CLOG_LSNS_PER_PAGE	(CLOG_XACTS_PER_PAGE / CLOG_XACTS_PER_LSN_GROUP)

#define GetLSNIndex(slotno, xid)	((slotno) * CLOG_LSNS_PER_PAGE + \
	((xid) % (TransactionId) CLOG_XACTS_PER_PAGE) / CLOG_XACTS_PER_LSN_GROUP)


/*
 * Link to shared-memory data structures for CLOG control
 */
static SlruCtlData ClogCtlData;

#define ClogCtl (&ClogCtlData)


static int	ZeroCLOGPage(int pageno, bool writeXlog);
static bool CLOGPagePrecedes(int page1, int page2);
static void WriteZeroPageXlogRec(int pageno);
static void WriteTruncateXlogRec(int pageno);

char *XidStatus_Name(XidStatus status)
{
	switch (status)
	{
	case TRANSACTION_STATUS_IN_PROGRESS:
		return "In-Progress";
	case TRANSACTION_STATUS_COMMITTED:
		return "Committed";
	case TRANSACTION_STATUS_ABORTED:
		return "Aborted";
	case TRANSACTION_STATUS_SUB_COMMITTED:
		return "Sub-Transaction Committed";

	default:
		return "Unknown";
	}
}

/*
 * Record the final state of a transaction in the commit log.
 *
 * lsn must be the WAL location of the commit record when recording an async
 * commit.	For a synchronous commit it can be InvalidXLogRecPtr, since the
 * caller guarantees the commit record is already flushed in that case.  It
 * should be InvalidXLogRecPtr for abort cases, too.
 *
 * NB: this is a low-level routine and is NOT the preferred entry point
 * for most uses; TransactionLogUpdate() in transam.c is the intended caller.
 */
void
TransactionIdSetStatus(TransactionId xid, XidStatus status, XLogRecPtr lsn)
{
	MIRRORED_LOCK_DECLARE;

	int			pageno = TransactionIdToPage(xid);
	int			byteno = TransactionIdToByte(xid);
	int			bshift = TransactionIdToBIndex(xid) * CLOG_BITS_PER_XACT;
	int			slotno;
	char	   *byteptr;
	char		byteval;
	int			debugStatus;

	Assert(status == TRANSACTION_STATUS_COMMITTED ||
		   status == TRANSACTION_STATUS_ABORTED ||
		   status == TRANSACTION_STATUS_SUB_COMMITTED);

	MIRRORED_LOCK;

	LWLockAcquire(CLogControlLock, LW_EXCLUSIVE);

	/*
	 * If we're doing an async commit (ie, lsn is valid), then we must wait
	 * for any active write on the page slot to complete.  Otherwise our
	 * update could reach disk in that write, which will not do since we
	 * mustn't let it reach disk until we've done the appropriate WAL flush.
	 * But when lsn is invalid, it's OK to scribble on a page while it is
	 * write-busy, since we don't care if the update reaches disk sooner than
	 * we think.  Hence, pass write_ok = XLogRecPtrIsInvalid(lsn).
	 */
	slotno = SimpleLruReadPage(ClogCtl, pageno, XLogRecPtrIsInvalid(lsn), xid);
	byteptr = ClogCtl->shared->page_buffer[slotno] + byteno;

	debugStatus = ((*byteptr >> bshift) & CLOG_XACT_BITMASK);
	if (debugStatus != 0 &&
		debugStatus != TRANSACTION_STATUS_SUB_COMMITTED &&
		debugStatus != status)
	{
		elog(FATAL,"TransactionIdSetStatus invalid for transaction %u current status '%s' (0x%x) and new status '%s' (0x%x)",
			 xid, 
			 XidStatus_Name(debugStatus),
			 debugStatus,
			 XidStatus_Name(status),
			 status);
	}


	/* Current state should be 0, subcommitted or target state */
	Assert(((*byteptr >> bshift) & CLOG_XACT_BITMASK) == 0 ||
		   ((*byteptr >> bshift) & CLOG_XACT_BITMASK) == TRANSACTION_STATUS_SUB_COMMITTED ||
		   ((*byteptr >> bshift) & CLOG_XACT_BITMASK) == status);

	/* note this assumes exclusive access to the clog page */
	byteval = *byteptr;
	byteval &= ~(((1 << CLOG_BITS_PER_XACT) - 1) << bshift);
	byteval |= (status << bshift);
	*byteptr = byteval;

	ClogCtl->shared->page_dirty[slotno] = true;

	/*
	 * Update the group LSN if the transaction completion LSN is higher.
	 *
	 * Note: lsn will be invalid when supplied during InRecovery processing,
	 * so we don't need to do anything special to avoid LSN updates during
	 * recovery. After recovery completes the next clog change will set the
	 * LSN correctly.
	 */
	if (!XLogRecPtrIsInvalid(lsn))
	{
		int			lsnindex = GetLSNIndex(slotno, xid);

		if (XLByteLT(ClogCtl->shared->group_lsn[lsnindex], lsn))
			ClogCtl->shared->group_lsn[lsnindex] = lsn;
	}

	LWLockRelease(CLogControlLock);

	MIRRORED_UNLOCK;
}

/*
 * Interrogate the state of a transaction in the commit log.
 *
 * Aside from the actual commit status, this function returns (into *lsn)
 * an LSN that is late enough to be able to guarantee that if we flush up to
 * that LSN then we will have flushed the transaction's commit record to disk.
 * The result is not necessarily the exact LSN of the transaction's commit
 * record!	For example, for long-past transactions (those whose clog pages
 * already migrated to disk), we'll return InvalidXLogRecPtr.  Also, because
 * we group transactions on the same clog page to conserve storage, we might
 * return the LSN of a later transaction that falls into the same group.
 *
 * NB: this is a low-level routine and is NOT the preferred entry point
 * for most uses; TransactionLogFetch() in transam.c is the intended caller.
 */
XidStatus
TransactionIdGetStatus(TransactionId xid, XLogRecPtr *lsn)
{
	MIRRORED_LOCK_DECLARE;

	int			pageno = TransactionIdToPage(xid);
	int			byteno = TransactionIdToByte(xid);
	int			bshift = TransactionIdToBIndex(xid) * CLOG_BITS_PER_XACT;
	int			slotno;
	int			lsnindex;
	char	   *byteptr;
	XidStatus	status;

	MIRRORED_LOCK;		// Since reading can cause eviction of another page (which requires writing).

	/* lock is acquired by SimpleLruReadPage_ReadOnly */

	slotno = SimpleLruReadPage_ReadOnly(ClogCtl, pageno, xid, NULL);
	byteptr = ClogCtl->shared->page_buffer[slotno] + byteno;

	status = (*byteptr >> bshift) & CLOG_XACT_BITMASK;

	lsnindex = GetLSNIndex(slotno, xid);
	*lsn = ClogCtl->shared->group_lsn[lsnindex];

	LWLockRelease(CLogControlLock);

	MIRRORED_UNLOCK;

	return status;
}


/* 
 * Interrogate the state of a transaction in the commit log, but 
 * allow situations were the status can not be retrieved.
 * 
 * The parameter valid is a pointer to a boolean. The value it points too 
 * will be "true" if the fuction's return value is a valid XidStatus, and
 * false if the function's return value is not a valid XidStatus.  
 *
 * return the XidStatus of the transaction (only 
 */
XidStatus
InRecoveryTransactionIdGetStatus(TransactionId xid, bool *valid)
{
  MIRRORED_LOCK_DECLARE;

  int                     pageno = TransactionIdToPage(xid);
  int                     byteno = TransactionIdToByte(xid);
  int                     bshift = TransactionIdToBIndex(xid) * CLOG_BITS_PER_XACT;
  int                     slotno;
  char       *byteptr;
  XidStatus       status;

  MIRRORED_LOCK;          // Since reading can cause eviction of another page (which requires writing).                                        

  /* lock is acquired by SimpleLruReadPage_ReadOnly */

  slotno = SimpleLruReadPage_ReadOnly(ClogCtl, pageno, xid, valid);

  if (valid != NULL && *valid == false)
    {
    /* The returned slotno is invalid. */ 
    MIRRORED_UNLOCK
    return TRANSACTION_STATUS_IN_PROGRESS;
    }

  byteptr = ClogCtl->shared->page_buffer[slotno] + byteno;

  status = (*byteptr >> bshift) & CLOG_XACT_BITMASK;

  LWLockRelease(CLogControlLock);

  MIRRORED_UNLOCK;

  return status;
}


/*
 * Find the next lowest transaction with a logged or recorded status.
 * I.e. One that does not have a status of default (0) -- i.e: in-progress.
 */
bool
CLOGScanForPrevStatus(
	TransactionId 			*indexXid,
	XidStatus				*status)
{
	MIRRORED_LOCK_DECLARE;

	TransactionId highXid;
	int pageno;
	TransactionId lowXid;
	int slotno;
	int byteno;
	int bshift;
	TransactionId xid;
	char *byteptr;

	*status = TRANSACTION_STATUS_IN_PROGRESS;	// Set it to something.

	if ((*indexXid) == InvalidTransactionId)
		return false;
	highXid = (*indexXid) - 1;
	if (highXid < FirstNormalTransactionId)
		return false;

	MIRRORED_LOCK;

	while (true)
	{
		pageno = TransactionIdToPage(highXid);

		/*
		 * Compute the xid floor for the page.
		 */
		lowXid = pageno * (TransactionId) CLOG_XACTS_PER_PAGE;
		if (lowXid == InvalidTransactionId)
			lowXid = FirstNormalTransactionId;

		LWLockAcquire(CLogControlLock, LW_EXCLUSIVE);

		/*
		 * Peek to see if page exists.
		 */
		if (!SimpleLruPageExists(ClogCtl, pageno))
		{
			LWLockRelease(CLogControlLock);

			MIRRORED_UNLOCK;

			*indexXid = InvalidTransactionId;
			*status = TRANSACTION_STATUS_IN_PROGRESS;	// Set it to something.
			return false;
		}
			
		slotno = SimpleLruReadPage(ClogCtl, pageno, false, highXid);

		for (xid = highXid; xid >= lowXid; xid--)
		{
			byteno = TransactionIdToByte(xid);
			bshift = TransactionIdToBIndex(xid) * CLOG_BITS_PER_XACT;
			byteptr = ClogCtl->shared->page_buffer[slotno] + byteno;
			*status = (*byteptr >> bshift) & CLOG_XACT_BITMASK;

			if (*status != TRANSACTION_STATUS_IN_PROGRESS)
			{
				LWLockRelease(CLogControlLock);

				MIRRORED_UNLOCK;

				*indexXid = xid;
				return true;
			}
		}

		LWLockRelease(CLogControlLock);

		if (lowXid == FirstNormalTransactionId)
		{
			MIRRORED_UNLOCK;

			*indexXid = InvalidTransactionId;
			*status = TRANSACTION_STATUS_IN_PROGRESS;	// Set it to something.
			return false;
		}
		
		highXid = lowXid - 1;	// Go to last xid of previous page.
	}

	MIRRORED_UNLOCK;

	return false;	// We'll never reach this.
}

/*
 * Determine the "age" of a transaction id.
 */
bool
CLOGTransactionIsOld(TransactionId xid)
{
	TransactionId nextXid;
	int pagesBack;

	if (ShmemVariableCache == NULL)
		return false;	// In case we are called very early in the life of the backend process, etc.

	nextXid = ShmemVariableCache->nextXid;

	if (nextXid < xid)
		return false;	// Not sure what is going on.

	pagesBack = (nextXid - xid) / CLOG_XACTS_PER_PAGE;

	/*
	 * Declare the transaction old if it is in the bottom older half of the hot CLOG cache window, or
	 * before the window.
	 */
	return (pagesBack > NUM_CLOG_BUFFERS/2);
}

/*
 * Initialization of shared memory for CLOG
 */
Size
CLOGShmemSize(void)
{
	return SimpleLruShmemSize(NUM_CLOG_BUFFERS, CLOG_LSNS_PER_PAGE);
}

void
CLOGShmemInit(void)
{
	ClogCtl->PagePrecedes = CLOGPagePrecedes;
	SimpleLruInit(ClogCtl, "CLOG Ctl", NUM_CLOG_BUFFERS, CLOG_LSNS_PER_PAGE,
				  CLogControlLock, "pg_clog");
}

/*
 * This func must be called ONCE on system install.  It creates
 * the initial CLOG segment.  (The CLOG directory is assumed to
 * have been created by the initdb shell script, and CLOGShmemInit
 * must have been called already.)
 */
void
BootStrapCLOG(void)
{
	MIRRORED_LOCK_DECLARE;

	int			slotno;

	MIRRORED_LOCK;

	LWLockAcquire(CLogControlLock, LW_EXCLUSIVE);

	/* Create and zero the first page of the commit log */
	slotno = ZeroCLOGPage(0, false);

	/* Make sure it's written out */
	SimpleLruWritePage(ClogCtl, slotno, NULL);
	Assert(!ClogCtl->shared->page_dirty[slotno]);

	LWLockRelease(CLogControlLock);

	MIRRORED_UNLOCK;
}

/*
 * Initialize (or reinitialize) a page of CLOG to zeroes.
 * If writeXlog is TRUE, also emit an XLOG record saying we did this.
 *
 * The page is not actually written, just set up in shared memory.
 * The slot number of the new page is returned.
 *
 * Control lock must be held at entry, and will be held at exit.
 */
static int
ZeroCLOGPage(int pageno, bool writeXlog)
{
	MIRRORED_LOCK_DECLARE;

	int			slotno;

	MIRRORED_LOCK;

	slotno = SimpleLruZeroPage(ClogCtl, pageno);

	if (writeXlog)
		WriteZeroPageXlogRec(pageno);

	MIRRORED_UNLOCK;

	return slotno;
}

/*
 * This must be called ONCE during postmaster or standalone-backend startup,
 * after StartupXLOG has initialized ShmemVariableCache->nextXid.
 */
void
StartupCLOG(void)
{
	MIRRORED_LOCK_DECLARE;

	TransactionId xid = ShmemVariableCache->nextXid;
	int			pageno = TransactionIdToPage(xid);

	MIRRORED_LOCK;

	LWLockAcquire(CLogControlLock, LW_EXCLUSIVE);

	/*
	 * Initialize our idea of the latest page number.
	 */
	ClogCtl->shared->latest_page_number = pageno;

	/*
	 * Zero out the remainder of the current clog page.  Under normal
	 * circumstances it should be zeroes already, but it seems at least
	 * theoretically possible that XLOG replay will have settled on a nextXID
	 * value that is less than the last XID actually used and marked by the
	 * previous database lifecycle (since subtransaction commit writes clog
	 * but makes no WAL entry).  Let's just be safe. (We need not worry about
	 * pages beyond the current one, since those will be zeroed when first
	 * used.  For the same reason, there is no need to do anything when
	 * nextXid is exactly at a page boundary; and it's likely that the
	 * "current" page doesn't exist yet in that case.)
	 */
	if (TransactionIdToPgIndex(xid) != 0)
	{
		int			byteno = TransactionIdToByte(xid);
		int			bshift = TransactionIdToBIndex(xid) * CLOG_BITS_PER_XACT;
		int			slotno;
		char	   *byteptr;

		slotno = SimpleLruReadPage(ClogCtl, pageno, false, xid);
		byteptr = ClogCtl->shared->page_buffer[slotno] + byteno;

		/* Zero so-far-unused positions in the current byte */
		*byteptr &= (1 << bshift) - 1;
		/* Zero the rest of the page */
		MemSet(byteptr + 1, 0, BLCKSZ - byteno - 1);

		ClogCtl->shared->page_dirty[slotno] = true;
	}

	LWLockRelease(CLogControlLock);

	MIRRORED_UNLOCK;
}

/*
 * This must be called ONCE during postmaster or standalone-backend shutdown
 */
void
ShutdownCLOG(void)
{
	MIRRORED_LOCK_DECLARE;

	MIRRORED_LOCK;

	/* Flush dirty CLOG pages to disk */
	TRACE_POSTGRESQL_CLOG_CHECKPOINT_START(false);
	SimpleLruFlush(ClogCtl, false);

	MIRRORED_UNLOCK;

	TRACE_POSTGRESQL_CLOG_CHECKPOINT_DONE(false);
}

/*
 * Perform a checkpoint --- either during shutdown, or on-the-fly
 */
void
CheckPointCLOG(void)
{
	MIRRORED_LOCK_DECLARE;

	MIRRORED_LOCK;

	/* Flush dirty CLOG pages to disk */
	TRACE_POSTGRESQL_CLOG_CHECKPOINT_START(true);
	SimpleLruFlush(ClogCtl, true);

	MIRRORED_UNLOCK;

	TRACE_POSTGRESQL_CLOG_CHECKPOINT_DONE(true);
}


/*
 * Make sure that CLOG has room for a newly-allocated XID.
 *
 * NB: this is called while holding XidGenLock.  We want it to be very fast
 * most of the time; even when it's not so fast, no actual I/O need happen
 * unless we're forced to write out a dirty clog or xlog page to make room
 * in shared memory.
 */
void
ExtendCLOG(TransactionId newestXact)
{
	MIRRORED_LOCK_DECLARE;

	int			pageno;

	/*
	 * No work except at first XID of a page.  But beware: just after
	 * wraparound, the first XID of page zero is FirstNormalTransactionId.
	 */
	if (TransactionIdToPgIndex(newestXact) != 0 &&
		!TransactionIdEquals(newestXact, FirstNormalTransactionId))
		return;

	pageno = TransactionIdToPage(newestXact);

	MIRRORED_LOCK;

	LWLockAcquire(CLogControlLock, LW_EXCLUSIVE);

	/* Zero the page and make an XLOG entry about it */
	ZeroCLOGPage(pageno, true);

	LWLockRelease(CLogControlLock);

	MIRRORED_UNLOCK;
}


/*
 * Remove all CLOG segments before the one holding the passed transaction ID
 *
 * Before removing any CLOG data, we must flush XLOG to disk, to ensure
 * that any recently-emitted HEAP_FREEZE records have reached disk; otherwise
 * a crash and restart might leave us with some unfrozen tuples referencing
 * removed CLOG data.  We choose to emit a special TRUNCATE XLOG record too.
 * Replaying the deletion from XLOG is not critical, since the files could
 * just as well be removed later, but doing so prevents a long-running hot
 * standby server from acquiring an unreasonably bloated CLOG directory.
 *
 * Since CLOG segments hold a large number of transactions, the opportunity to
 * actually remove a segment is fairly rare, and so it seems best not to do
 * the XLOG flush unless we have confirmed that there is a removable segment.
 */
void
TruncateCLOG(TransactionId oldestXact)
{
	MIRRORED_LOCK_DECLARE;

	int			cutoffPage;

	/*
	 * The cutoff point is the start of the segment containing oldestXact. We
	 * pass the *page* containing oldestXact to SimpleLruTruncate.
	 */
	cutoffPage = TransactionIdToPage(oldestXact);

	MIRRORED_LOCK;

	/* Check to see if there's any files that could be removed */
	if (!SlruScanDirectory(ClogCtl, cutoffPage, false))
	{
		MIRRORED_UNLOCK;
		return;					/* nothing to remove */
	}

	/* Write XLOG record and flush XLOG to disk */
	WriteTruncateXlogRec(cutoffPage);

	/* Now we can remove the old CLOG segment(s) */
	SimpleLruTruncate(ClogCtl, cutoffPage);

	MIRRORED_UNLOCK;
}


/*
 * Decide which of two CLOG page numbers is "older" for truncation purposes.
 *
 * We need to use comparison of TransactionIds here in order to do the right
 * thing with wraparound XID arithmetic.  However, if we are asked about
 * page number zero, we don't want to hand InvalidTransactionId to
 * TransactionIdPrecedes: it'll get weird about permanent xact IDs.  So,
 * offset both xids by FirstNormalTransactionId to avoid that.
 */
static bool
CLOGPagePrecedes(int page1, int page2)
{
	TransactionId xid1;
	TransactionId xid2;

	xid1 = ((TransactionId) page1) * CLOG_XACTS_PER_PAGE;
	xid1 += FirstNormalTransactionId;
	xid2 = ((TransactionId) page2) * CLOG_XACTS_PER_PAGE;
	xid2 += FirstNormalTransactionId;

	return TransactionIdPrecedes(xid1, xid2);
}


/*
 * Write a ZEROPAGE xlog record
 */
static void
WriteZeroPageXlogRec(int pageno)
{
	XLogRecData rdata;

	rdata.data = (char *) (&pageno);
	rdata.len = sizeof(int);
	rdata.buffer = InvalidBuffer;
	rdata.next = NULL;
	(void) XLogInsert(RM_CLOG_ID, CLOG_ZEROPAGE, &rdata);
}

/*
 * Write a TRUNCATE xlog record
 *
 * We must flush the xlog record to disk before returning --- see notes
 * in TruncateCLOG().
 */
static void
WriteTruncateXlogRec(int pageno)
{
	XLogRecData rdata;
	XLogRecPtr	recptr;

	rdata.data = (char *) (&pageno);
	rdata.len = sizeof(int);
	rdata.buffer = InvalidBuffer;
	rdata.next = NULL;
	recptr = XLogInsert(RM_CLOG_ID, CLOG_TRUNCATE, &rdata);
	XLogFlush(recptr);
}

/*
 * CLOG resource manager's routines
 */
void
clog_redo(XLogRecPtr beginLoc, XLogRecPtr lsn, XLogRecord *record)
{
	MIRRORED_LOCK_DECLARE;

	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	MIRRORED_LOCK;

	if (info == CLOG_ZEROPAGE)
	{
		int			pageno;
		int			slotno;

		memcpy(&pageno, XLogRecGetData(record), sizeof(int));

		LWLockAcquire(CLogControlLock, LW_EXCLUSIVE);

		slotno = ZeroCLOGPage(pageno, false);
		SimpleLruWritePage(ClogCtl, slotno, NULL);
		Assert(!ClogCtl->shared->page_dirty[slotno]);

		LWLockRelease(CLogControlLock);
	}
	else if (info == CLOG_TRUNCATE)
	{
		int			pageno;

		memcpy(&pageno, XLogRecGetData(record), sizeof(int));

		/*
		 * During XLOG replay, latest_page_number isn't set up yet; insert a
		 * suitable value to bypass the sanity test in SimpleLruTruncate.
		 */
		ClogCtl->shared->latest_page_number = pageno;

		SimpleLruTruncate(ClogCtl, pageno);
	}
	else
		elog(PANIC, "clog_redo: unknown op code %u", info);

	MIRRORED_UNLOCK;
}

void
clog_desc(StringInfo buf, XLogRecPtr beginLoc, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;
	char		*rec = XLogRecGetData(record);

	if (info == CLOG_ZEROPAGE)
	{
		int			pageno;

		memcpy(&pageno, rec, sizeof(int));
		appendStringInfo(buf, "zeropage: %d", pageno);
	}
	else if (info == CLOG_TRUNCATE)
	{
		int			pageno;

		memcpy(&pageno, rec, sizeof(int));
		appendStringInfo(buf, "truncate before: %d", pageno);
	}
	else
		appendStringInfo(buf, "UNKNOWN");
}
