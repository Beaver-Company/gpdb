/*-------------------------------------------------------------------------
 *
 * nodeFunctionscan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeFunctionscan.h,v 1.12 2008/10/01 19:51:49 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEFUNCTIONSCAN_H
#define NODEFUNCTIONSCAN_H

#include "executor/executor.h"

extern int	ExecCountSlotsFunctionScan(FunctionScan *node);
extern FunctionScanState *ExecInitFunctionScan(FunctionScan *node, EState *estate, int eflags);
extern TupleTableSlot *ExecFunctionScan(FunctionScanState *node);
extern void ExecEndFunctionScan(FunctionScanState *node);
extern void ExecFunctionReScan(FunctionScanState *node, ExprContext *exprCtxt);
extern void ExecEagerFreeFunctionScan(FunctionScanState *node);

#endif   /* NODEFUNCTIONSCAN_H */
