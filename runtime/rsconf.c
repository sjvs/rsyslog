/* rsconf.c - the rsyslog configuration system.
 *
 * Module begun 2011-04-19 by Rainer Gerhards
 *
 * Copyright 2011 by Rainer Gerhards and Adiscon GmbH.
 *
 * This file is part of the rsyslog runtime library.
 *
 * The rsyslog runtime library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * The rsyslog runtime library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with the rsyslog runtime library.  If not, see <http://www.gnu.org/licenses/>.
 *
 * A copy of the GPL can be found in the file "COPYING" in this distribution.
 * A copy of the LGPL can be found in the file "COPYING.LESSER" in this distribution.
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "rsyslog.h"
#include "obj.h"
#include "srUtils.h"
#include "ruleset.h"
#include "modules.h"
#include "conf.h"
#include "queue.h"
#include "rsconf.h"
#include "cfsysline.h"
#include "errmsg.h"
#include "action.h"
#include "glbl.h"
#include "unicode-helper.h"

/* static data */
DEFobjStaticHelpers
DEFobjCurrIf(ruleset)
DEFobjCurrIf(module)
DEFobjCurrIf(conf)
DEFobjCurrIf(errmsg)
DEFobjCurrIf(glbl)

/* exported static data */
rsconf_t *runConf = NULL;/* the currently running config */
rsconf_t *loadConf = NULL;/* the config currently being loaded (no concurrent config load supported!) */

/* Standard-Constructor
 */
BEGINobjConstruct(rsconf) /* be sure to specify the object type also in END macro! */
	pThis->globals.bDebugPrintTemplateList = 1;
	pThis->globals.bDebugPrintModuleList = 1;
	pThis->globals.bDebugPrintCfSysLineHandlerList = 1;
	pThis->globals.bLogStatusMsgs = DFLT_bLogStatusMsgs;
	pThis->globals.bErrMsgToStderr = 1;
	pThis->templates.root = NULL;
	pThis->templates.last = NULL;
	pThis->templates.lastStatic = NULL;
	pThis->actions.nbrActions = 0;
	CHKiRet(llInit(&pThis->rulesets.llRulesets, rulesetDestructForLinkedList,
			rulesetKeyDestruct, strcasecmp));
	/* queue params */
	pThis->globals.mainQ.iMainMsgQueueSize = 10000;
	pThis->globals.mainQ.iMainMsgQHighWtrMark = 8000;
	pThis->globals.mainQ.iMainMsgQLowWtrMark = 2000;
	pThis->globals.mainQ.iMainMsgQDiscardMark = 9800;
	pThis->globals.mainQ.iMainMsgQDiscardSeverity = 8;
	pThis->globals.mainQ.iMainMsgQueueNumWorkers = 1;
	pThis->globals.mainQ.MainMsgQueType = QUEUETYPE_FIXED_ARRAY;
	pThis->globals.mainQ.pszMainMsgQFName = NULL;
	pThis->globals.mainQ.iMainMsgQueMaxFileSize = 1024*1024;
	pThis->globals.mainQ.iMainMsgQPersistUpdCnt = 0;
	pThis->globals.mainQ.bMainMsgQSyncQeueFiles = 0;
	pThis->globals.mainQ.iMainMsgQtoQShutdown = 1500;
	pThis->globals.mainQ.iMainMsgQtoActShutdown = 1000;
	pThis->globals.mainQ.iMainMsgQtoEnq = 2000;
	pThis->globals.mainQ.iMainMsgQtoWrkShutdown = 60000;
	pThis->globals.mainQ.iMainMsgQWrkMinMsgs = 100;
	pThis->globals.mainQ.iMainMsgQDeqSlowdown = 0;
	pThis->globals.mainQ.iMainMsgQueMaxDiskSpace = 0;
	pThis->globals.mainQ.iMainMsgQueDeqBatchSize = 32;
	pThis->globals.mainQ.bMainMsgQSaveOnShutdown = 1;
	pThis->globals.mainQ.iMainMsgQueueDeqtWinFromHr = 0;
	pThis->globals.mainQ.iMainMsgQueueDeqtWinToHr = 25;
	/* end queue params */
finalize_it:
ENDobjConstruct(rsconf)


/* ConstructionFinalizer
 */
rsRetVal rsconfConstructFinalize(rsconf_t __attribute__((unused)) *pThis)
{
	DEFiRet;
	ISOBJ_TYPE_assert(pThis, rsconf);
	RETiRet;
}


/* destructor for the rsconf object */
BEGINobjDestruct(rsconf) /* be sure to specify the object type also in END and CODESTART macros! */
CODESTARTobjDestruct(rsconf)
	free(pThis->globals.mainQ.pszMainMsgQFName);
	llDestroy(&(pThis->rulesets.llRulesets));
ENDobjDestruct(rsconf)


/* DebugPrint support for the rsconf object */
BEGINobjDebugPrint(rsconf) /* be sure to specify the object type also in END and CODESTART macros! */
	dbgprintf("configuration object %p\n", pThis);
	dbgprintf("Global Settings:\n");
	dbgprintf("  bDebugPrintTemplateList.............: %d\n",
		  pThis->globals.bDebugPrintTemplateList);
	dbgprintf("  bDebugPrintModuleList               : %d\n",
		  pThis->globals.bDebugPrintModuleList);
	dbgprintf("  bDebugPrintCfSysLineHandlerList.....: %d\n",
		  pThis->globals.bDebugPrintCfSysLineHandlerList);
	dbgprintf("  bLogStatusMsgs                      : %d\n",
		  pThis->globals.bLogStatusMsgs);
	dbgprintf("  bErrMsgToStderr.....................: %d\n",
		  pThis->globals.bErrMsgToStderr);
	ruleset.DebugPrintAll(pThis);
	DBGPRINTF("\n");
	if(pThis->globals.bDebugPrintTemplateList)
		tplPrintList(pThis);
	if(pThis->globals.bDebugPrintModuleList)
		module.PrintList();
	if(pThis->globals.bDebugPrintCfSysLineHandlerList)
		dbgPrintCfSysLineHandlers();
	// TODO: The following code needs to be "streamlined", so far just moved over...
	DBGPRINTF("Main queue size %d messages.\n", pThis->globals.mainQ.iMainMsgQueueSize);
	DBGPRINTF("Main queue worker threads: %d, wThread shutdown: %d, Perists every %d updates.\n",
		  pThis->globals.mainQ.iMainMsgQueueNumWorkers, pThis->globals.mainQ.iMainMsgQtoWrkShutdown, pThis->globals.mainQ.iMainMsgQPersistUpdCnt);
	DBGPRINTF("Main queue timeouts: shutdown: %d, action completion shutdown: %d, enq: %d\n",
		   pThis->globals.mainQ.iMainMsgQtoQShutdown, pThis->globals.mainQ.iMainMsgQtoActShutdown, pThis->globals.mainQ.iMainMsgQtoEnq);
	DBGPRINTF("Main queue watermarks: high: %d, low: %d, discard: %d, discard-severity: %d\n",
		   pThis->globals.mainQ.iMainMsgQHighWtrMark, pThis->globals.mainQ.iMainMsgQLowWtrMark, pThis->globals.mainQ.iMainMsgQDiscardMark, pThis->globals.mainQ.iMainMsgQDiscardSeverity);
	DBGPRINTF("Main queue save on shutdown %d, max disk space allowed %lld\n",
		   pThis->globals.mainQ.bMainMsgQSaveOnShutdown, pThis->globals.mainQ.iMainMsgQueMaxDiskSpace);
	/* TODO: add
	iActionRetryCount = 0;
	iActionRetryInterval = 30000;
       static int iMainMsgQtoWrkMinMsgs = 100;
	static int iMainMsgQbSaveOnShutdown = 1;
	iMainMsgQueMaxDiskSpace = 0;
	setQPROP(qqueueSetiMinMsgsPerWrkr, "$MainMsgQueueWorkerThreadMinimumMessages", 100);
	setQPROP(qqueueSetbSaveOnShutdown, "$MainMsgQueueSaveOnShutdown", 1);
	 */
	DBGPRINTF("Work Directory: '%s'.\n", glbl.GetWorkDir());
CODESTARTobjDebugPrint(rsconf)
ENDobjDebugPrint(rsconf)


/* Activate an already-loaded configuration. The configuration will become
 * the new running conf (if successful). Note that in theory this method may
 * be called when there already is a running conf. In practice, the current
 * version of rsyslog does not support this. Future versions probably will.
 * Begun 2011-04-20, rgerhards
 */
rsRetVal
activate(rsconf_t *cnf)
{
	DEFiRet;
	runConf = cnf;
	dbgprintf("configuration %p activated\n", cnf);
	RETiRet;
}


/* -------------------- some legacy config handlers --------------------
 * TODO: move to conf.c?
 */

/* this method is needed to shuffle the current conf object down to the
 * IncludeConfig handler.
 */
static rsRetVal
doModLoad(void *pVal, uchar *pNewVal)
{
	DEFiRet;
	iRet = conf.doModLoad(ourConf, pVal, pNewVal);
	free(pNewVal);
	RETiRet;
}

/* legacy config system: set the action resume interval */
static rsRetVal setActionResumeInterval(void __attribute__((unused)) *pVal, int iNewVal)
{
	return actionSetGlobalResumeInterval(iNewVal);
}


/* this method is needed to shuffle the current conf object down to the
 * IncludeConfig handler.
 */
static rsRetVal
doIncludeLine(void *pVal, uchar *pNewVal)
{
	DEFiRet;
	iRet = conf.doIncludeLine(ourConf, pVal, pNewVal);
	free(pNewVal);
	RETiRet;
}


/* set the processes umask (upon configuration request) */
static rsRetVal
setUmask(void __attribute__((unused)) *pVal, int iUmask)
{
#warning this *really* needs to be done differently!
	umask(iUmask);
	DBGPRINTF("umask set to 0%3.3o.\n", iUmask);

	return RS_RET_OK;
}


/* set the maximum message size */
static rsRetVal setMaxMsgSize(void __attribute__((unused)) *pVal, long iNewVal)
{
	return glbl.SetMaxLine(iNewVal);
}


/* Switch the default ruleset (that, what servcies bind to if nothing specific
 * is specified).
 * rgerhards, 2009-06-12
 */
static rsRetVal
setDefaultRuleset(void __attribute__((unused)) *pVal, uchar *pszName)
{
	DEFiRet;

	CHKiRet(ruleset.SetDefaultRuleset(ourConf, pszName));

finalize_it:
	free(pszName); /* no longer needed */
	RETiRet;
}


/* Switch to either an already existing rule set or start a new one. The
 * named rule set becomes the new "current" rule set (what means that new
 * actions are added to it).
 * rgerhards, 2009-06-12
 */
static rsRetVal
setCurrRuleset(void __attribute__((unused)) *pVal, uchar *pszName)
{
	ruleset_t *pRuleset;
	rsRetVal localRet;
	DEFiRet;

	localRet = ruleset.SetCurrRuleset(ourConf, pszName);

	if(localRet == RS_RET_NOT_FOUND) {
		DBGPRINTF("begin new current rule set '%s'\n", pszName);
		CHKiRet(ruleset.Construct(&pRuleset));
		CHKiRet(ruleset.SetName(ourConf, pRuleset, pszName));
		CHKiRet(ruleset.ConstructFinalize(ourConf, pRuleset));
	} else {
		ABORT_FINALIZE(localRet);
	}

finalize_it:
	free(pszName); /* no longer needed */
	RETiRet;
}


/* set the main message queue mode
 * rgerhards, 2008-01-03
 */
static rsRetVal setMainMsgQueType(void __attribute__((unused)) *pVal, uchar *pszType)
{
	DEFiRet;

	if (!strcasecmp((char *) pszType, "fixedarray")) {
		loadConf->globals.mainQ.MainMsgQueType = QUEUETYPE_FIXED_ARRAY;
		DBGPRINTF("main message queue type set to FIXED_ARRAY\n");
	} else if (!strcasecmp((char *) pszType, "linkedlist")) {
		loadConf->globals.mainQ.MainMsgQueType = QUEUETYPE_LINKEDLIST;
		DBGPRINTF("main message queue type set to LINKEDLIST\n");
	} else if (!strcasecmp((char *) pszType, "disk")) {
		loadConf->globals.mainQ.MainMsgQueType = QUEUETYPE_DISK;
		DBGPRINTF("main message queue type set to DISK\n");
	} else if (!strcasecmp((char *) pszType, "direct")) {
		loadConf->globals.mainQ.MainMsgQueType = QUEUETYPE_DIRECT;
		DBGPRINTF("main message queue type set to DIRECT (no queueing at all)\n");
	} else {
		errmsg.LogError(0, RS_RET_INVALID_PARAMS, "unknown mainmessagequeuetype parameter: %s", (char *) pszType);
		iRet = RS_RET_INVALID_PARAMS;
	}
	free(pszType); /* no longer needed */

	RETiRet;
}


/* -------------------- end legacy config handlers -------------------- */


/* set the processes max number ob files (upon configuration request)
 * 2009-04-14 rgerhards
 */
static rsRetVal setMaxFiles(void __attribute__((unused)) *pVal, int iFiles)
{
// TODO this must use a local var, then carry out action during activate!
	struct rlimit maxFiles;
	char errStr[1024];
	DEFiRet;

	maxFiles.rlim_cur = iFiles;
	maxFiles.rlim_max = iFiles;

	if(setrlimit(RLIMIT_NOFILE, &maxFiles) < 0) {
		/* NOTE: under valgrind, we seem to be unable to extend the size! */
		rs_strerror_r(errno, errStr, sizeof(errStr));
		errmsg.LogError(0, RS_RET_ERR_RLIM_NOFILE, "could not set process file limit to %d: %s [kernel max %ld]",
				iFiles, errStr, (long) maxFiles.rlim_max);
		ABORT_FINALIZE(RS_RET_ERR_RLIM_NOFILE);
	}
#ifdef USE_UNLIMITED_SELECT
	glbl.SetFdSetSize(howmany(iFiles, __NFDBITS) * sizeof (fd_mask));
#endif
	DBGPRINTF("Max number of files set to %d [kernel max %ld].\n", iFiles, (long) maxFiles.rlim_max);

finalize_it:
	RETiRet;
}


/* legac config system: reset config variables to default values.  */
static rsRetVal resetConfigVariables(uchar __attribute__((unused)) *pp, void __attribute__((unused)) *pVal)
{
	loadConf->globals.bLogStatusMsgs = DFLT_bLogStatusMsgs;
	loadConf->globals.bDebugPrintTemplateList = 1;
	loadConf->globals.bDebugPrintCfSysLineHandlerList = 1;
	loadConf->globals.bDebugPrintModuleList = 1;
	loadConf->globals.bAbortOnUncleanConfig = 0;
	loadConf->globals.bReduceRepeatMsgs = 0;
	free(loadConf->globals.mainQ.pszMainMsgQFName);
	loadConf->globals.mainQ.pszMainMsgQFName = NULL;
	loadConf->globals.mainQ.iMainMsgQueueSize = 10000;
	loadConf->globals.mainQ.iMainMsgQHighWtrMark = 8000;
	loadConf->globals.mainQ.iMainMsgQLowWtrMark = 2000;
	loadConf->globals.mainQ.iMainMsgQDiscardMark = 9800;
	loadConf->globals.mainQ.iMainMsgQDiscardSeverity = 8;
	loadConf->globals.mainQ.iMainMsgQueMaxFileSize = 1024 * 1024;
	loadConf->globals.mainQ.iMainMsgQueueNumWorkers = 1;
	loadConf->globals.mainQ.iMainMsgQPersistUpdCnt = 0;
	loadConf->globals.mainQ.bMainMsgQSyncQeueFiles = 0;
	loadConf->globals.mainQ.iMainMsgQtoQShutdown = 1500;
	loadConf->globals.mainQ.iMainMsgQtoActShutdown = 1000;
	loadConf->globals.mainQ.iMainMsgQtoEnq = 2000;
	loadConf->globals.mainQ.iMainMsgQtoWrkShutdown = 60000;
	loadConf->globals.mainQ.iMainMsgQWrkMinMsgs = 100;
	loadConf->globals.mainQ.iMainMsgQDeqSlowdown = 0;
	loadConf->globals.mainQ.bMainMsgQSaveOnShutdown = 1;
	loadConf->globals.mainQ.MainMsgQueType = QUEUETYPE_FIXED_ARRAY;
	loadConf->globals.mainQ.iMainMsgQueMaxDiskSpace = 0;
	loadConf->globals.mainQ.iMainMsgQueDeqBatchSize = 32;

	return RS_RET_OK;
}


/* legacy config system: set the action resume interval */
static rsRetVal
setModDir(void __attribute__((unused)) *pVal, uchar* pszNewVal)
{
	DEFiRet;
	iRet = module.SetModDir(pszNewVal);
	free(pszNewVal);
	RETiRet;
}


/* intialize the legacy config system */
static inline rsRetVal 
initLegacyConf(void)
{
	DEFiRet;
	ruleset_t *pRuleset;

	/* construct the default ruleset */
	ruleset.Construct(&pRuleset);
	ruleset.SetName(loadConf, pRuleset, UCHAR_CONSTANT("RSYSLOG_DefaultRuleset"));
	ruleset.ConstructFinalize(loadConf, pRuleset);

	/* now register config handlers */
	CHKiRet(regCfSysLineHdlr((uchar *)"sleep", 0, eCmdHdlrGoneAway,
		NULL, NULL, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"logrsyslogstatusmessages", 0, eCmdHdlrBinary,
		NULL, &loadConf->globals.bLogStatusMsgs, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"errormessagestostderr", 0, eCmdHdlrBinary,
		NULL, &loadConf->globals.bErrMsgToStderr, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"abortonuncleanconfig", 0, eCmdHdlrBinary,
		NULL, &loadConf->globals.bAbortOnUncleanConfig, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"repeatedmsgreduction", 0, eCmdHdlrBinary,
		NULL, &loadConf->globals.bReduceRepeatMsgs, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"debugprinttemplatelist", 0, eCmdHdlrBinary,
		NULL, &(loadConf->globals.bDebugPrintTemplateList), NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"debugprintmodulelist", 0, eCmdHdlrBinary,
		NULL, &(loadConf->globals.bDebugPrintModuleList), NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"debugprintcfsyslinehandlerlist", 0, eCmdHdlrBinary,
		 NULL, &(loadConf->globals.bDebugPrintCfSysLineHandlerList), NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"privdroptouser", 0, eCmdHdlrUID,
		NULL, &loadConf->globals.uidDropPriv, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"privdroptouserid", 0, eCmdHdlrInt,
		NULL, &loadConf->globals.uidDropPriv, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"privdroptogroup", 0, eCmdHdlrGID,
		NULL, &loadConf->globals.gidDropPriv, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"privdroptogroupid", 0, eCmdHdlrGID,
		NULL, &loadConf->globals.gidDropPriv, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"generateconfiggraph", 0, eCmdHdlrGetWord,
		NULL, &loadConf->globals.pszConfDAGFile, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"maxopenfiles", 0, eCmdHdlrInt,
		setMaxFiles, NULL, NULL, eConfObjGlobal));

	CHKiRet(regCfSysLineHdlr((uchar *)"actionresumeinterval", 0, eCmdHdlrInt,
		setActionResumeInterval, NULL, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"modload", 0, eCmdHdlrCustomHandler,
		doModLoad, NULL, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"includeconfig", 0, eCmdHdlrCustomHandler,
		doIncludeLine, NULL, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"umask", 0, eCmdHdlrFileCreateMode,
		setUmask, NULL, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"maxmessagesize", 0, eCmdHdlrSize,
		setMaxMsgSize, NULL, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"defaultruleset", 0, eCmdHdlrGetWord,
		setDefaultRuleset, NULL, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"ruleset", 0, eCmdHdlrGetWord,
		setCurrRuleset, NULL, NULL, eConfObjGlobal));

	/* handler for "larger" config statements (tie into legacy conf system) */
	CHKiRet(regCfSysLineHdlr((uchar *)"template", 0, eCmdHdlrCustomHandler,
		conf.doNameLine, (void*)DIR_TEMPLATE, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"outchannel", 0, eCmdHdlrCustomHandler,
		conf.doNameLine, (void*)DIR_OUTCHANNEL, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"allowedsender", 0, eCmdHdlrCustomHandler,
		conf.doNameLine, (void*)DIR_ALLOWEDSENDER, NULL, eConfObjGlobal));

	/* the following are parameters for the main message queue. I have the
	 * strong feeling that this needs to go to a different space, but that
	 * feeling may be wrong - we'll see how things evolve.
	 * rgerhards, 2011-04-21
	 */
	CHKiRet(regCfSysLineHdlr((uchar *)"mainmsgqueuefilename", 0, eCmdHdlrGetWord,
		NULL, &loadConf->globals.mainQ.pszMainMsgQFName, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"mainmsgqueuesize", 0, eCmdHdlrInt,
		NULL, &loadConf->globals.mainQ.iMainMsgQueueSize, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"mainmsgqueuehighwatermark", 0, eCmdHdlrInt,
		NULL, &loadConf->globals.mainQ.iMainMsgQHighWtrMark, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"mainmsgqueuelowwatermark", 0, eCmdHdlrInt,
		NULL, &loadConf->globals.mainQ.iMainMsgQLowWtrMark, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"mainmsgqueuediscardmark", 0, eCmdHdlrInt,
		NULL, &loadConf->globals.mainQ.iMainMsgQDiscardMark, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"mainmsgqueuediscardseverity", 0, eCmdHdlrSeverity,
		NULL, &loadConf->globals.mainQ.iMainMsgQDiscardSeverity, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"mainmsgqueuecheckpointinterval", 0, eCmdHdlrInt,
		NULL, &loadConf->globals.mainQ.iMainMsgQPersistUpdCnt, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"mainmsgqueuesyncqueuefiles", 0, eCmdHdlrBinary,
		NULL, &loadConf->globals.mainQ.bMainMsgQSyncQeueFiles, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"mainmsgqueuetype", 0, eCmdHdlrGetWord,
		setMainMsgQueType, NULL, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"mainmsgqueueworkerthreads", 0, eCmdHdlrInt,
		NULL, &loadConf->globals.mainQ.iMainMsgQueueNumWorkers, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"mainmsgqueuetimeoutshutdown", 0, eCmdHdlrInt,
		NULL, &loadConf->globals.mainQ.iMainMsgQtoQShutdown, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"mainmsgqueuetimeoutactioncompletion", 0, eCmdHdlrInt,
		NULL, &loadConf->globals.mainQ.iMainMsgQtoActShutdown, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"mainmsgqueuetimeoutenqueue", 0, eCmdHdlrInt,
		NULL, &loadConf->globals.mainQ.iMainMsgQtoEnq, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"mainmsgqueueworkertimeoutthreadshutdown", 0, eCmdHdlrInt,
		NULL, &loadConf->globals.mainQ.iMainMsgQtoWrkShutdown, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"mainmsgqueuedequeueslowdown", 0, eCmdHdlrInt,
		NULL, &loadConf->globals.mainQ.iMainMsgQDeqSlowdown, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"mainmsgqueueworkerthreadminimummessages", 0, eCmdHdlrInt,
		NULL, &loadConf->globals.mainQ.iMainMsgQWrkMinMsgs, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"mainmsgqueuemaxfilesize", 0, eCmdHdlrSize,
		NULL, &loadConf->globals.mainQ.iMainMsgQueMaxFileSize, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"mainmsgqueuedequeuebatchsize", 0, eCmdHdlrSize,
		NULL, &loadConf->globals.mainQ.iMainMsgQueDeqBatchSize, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"mainmsgqueuemaxdiskspace", 0, eCmdHdlrSize,
		NULL, &loadConf->globals.mainQ.iMainMsgQueMaxDiskSpace, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"mainmsgqueuesaveonshutdown", 0, eCmdHdlrBinary,
		NULL, &loadConf->globals.mainQ.bMainMsgQSaveOnShutdown, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"mainmsgqueuedequeuetimebegin", 0, eCmdHdlrInt,
		NULL, &loadConf->globals.mainQ.iMainMsgQueueDeqtWinFromHr, NULL, eConfObjGlobal));
	CHKiRet(regCfSysLineHdlr((uchar *)"mainmsgqueuedequeuetimeend", 0, eCmdHdlrInt,
		NULL, &loadConf->globals.mainQ.iMainMsgQueueDeqtWinToHr, NULL, eConfObjGlobal));
	/* moddir is a bit hard problem -- because it actually needs to
	 * modify a setting that is specific to module.c. The important point
	 * is that this action MUST actually be carried out during config load,
	 * because we must load modules in order to get their config extensions
	 * (no way around).
	 * TODO: think about a clean solution
	 */
	CHKiRet(regCfSysLineHdlr((uchar *)"moddir", 0, eCmdHdlrGetWord,
		setModDir, NULL, NULL, eConfObjGlobal));

	/* finally, the reset handler */
	CHKiRet(regCfSysLineHdlr((uchar *)"resetconfigvariables", 1, eCmdHdlrCustomHandler,
		resetConfigVariables, NULL, NULL, eConfObjGlobal));

finalize_it:
	RETiRet;
}


/* Load a configuration. This will do all necessary steps to create
 * the in-memory representation of the configuration, including support
 * for multiple configuration languages.
 * Note that to support the legacy language we must provide some global
 * object that holds the currently-being-loaded config ptr.
 * Begun 2011-04-20, rgerhards
 */
rsRetVal
load(rsconf_t **cnf, uchar *confFile)
{
	rsRetVal localRet;
	int iNbrActions;
	int bHadConfigErr = 0;
	char cbuf[BUFSIZ];
	DEFiRet;

	CHKiRet(rsconfConstruct(&loadConf));
ourConf = loadConf; // TODO: remove, once ourConf is gone!
dbgprintf("XXXX: loadConf is %p\n", loadConf);

	CHKiRet(initLegacyConf());

#if 0
dbgprintf("XXXX: 2, conf=%p\n", conf.processConfFile);
	/* open the configuration file */
	localRet = conf.processConfFile(loadConf, confFile);
	CHKiRet(conf.GetNbrActActions(loadConf, &iNbrActions));

dbgprintf("XXXX: 4\n");
	if(localRet != RS_RET_OK) {
		errmsg.LogError(0, localRet, "CONFIG ERROR: could not interpret master config file '%s'.", confFile);
		bHadConfigErr = 1;
	} else if(iNbrActions == 0) {
		errmsg.LogError(0, RS_RET_NO_ACTIONS, "CONFIG ERROR: there are no active actions configured. Inputs will "
			 "run, but no output whatsoever is created.");
		bHadConfigErr = 1;
	}

dbgprintf("XXXX: 10\n");
	if((localRet != RS_RET_OK && localRet != RS_RET_NONFATAL_CONFIG_ERR) || iNbrActions == 0) {

dbgprintf("XXXX: 20\n");
		/* rgerhards: this code is executed to set defaults when the
		 * config file could not be opened. We might think about
		 * abandoning the run in this case - but this, too, is not
		 * very clever... So we stick with what we have.
		 * We ignore any errors while doing this - we would be lost anyhow...
		 */
		errmsg.LogError(0, NO_ERRCODE, "EMERGENCY CONFIGURATION ACTIVATED - fix rsyslog config file!");

		/* note: we previously used _POSIY_TTY_NAME_MAX+1, but this turned out to be
		 * too low on linux... :-S   -- rgerhards, 2008-07-28
		 */
		char szTTYNameBuf[128];
		rule_t *pRule = NULL; /* initialization to NULL is *vitally* important! */
		conf.cfline(loadConf, UCHAR_CONSTANT("*.ERR\t" _PATH_CONSOLE), &pRule);
		conf.cfline(loadConf, UCHAR_CONSTANT("syslog.*\t" _PATH_CONSOLE), &pRule);
		conf.cfline(loadConf, UCHAR_CONSTANT("*.PANIC\t*"), &pRule);
		conf.cfline(loadConf, UCHAR_CONSTANT("syslog.*\troot"), &pRule);
		if(ttyname_r(0, szTTYNameBuf, sizeof(szTTYNameBuf)) == 0) {
			snprintf(cbuf,sizeof(cbuf), "*.*\t%s", szTTYNameBuf);
			conf.cfline(loadConf, (uchar*)cbuf, &pRule);
		} else {
			DBGPRINTF("error %d obtaining controlling terminal, not using that emergency rule\n", errno);
		}
		ruleset.AddRule(loadConf, ruleset.GetCurrent(loadConf), &pRule);
	}

#endif


	/* all OK, pass loaded conf to caller */
	*cnf = loadConf;
// TODO: enable this once all config code is moved to here!	loadConf = NULL;

	dbgprintf("rsyslog finished loading initial config %p\n", loadConf);
//	rsconfDebugPrint(loadConf);

finalize_it:
	RETiRet;
}


/* queryInterface function
 */
BEGINobjQueryInterface(rsconf)
CODESTARTobjQueryInterface(rsconf)
	if(pIf->ifVersion != rsconfCURR_IF_VERSION) { /* check for current version, increment on each change */
		ABORT_FINALIZE(RS_RET_INTERFACE_NOT_SUPPORTED);
	}

	/* ok, we have the right interface, so let's fill it
	 * Please note that we may also do some backwards-compatibility
	 * work here (if we can support an older interface version - that,
	 * of course, also affects the "if" above).
	 */
	pIf->Construct = rsconfConstruct;
	pIf->ConstructFinalize = rsconfConstructFinalize;
	pIf->Destruct = rsconfDestruct;
	pIf->DebugPrint = rsconfDebugPrint;
	pIf->Load = load;
	pIf->Activate = activate;
finalize_it:
ENDobjQueryInterface(rsconf)


/* Initialize the rsconf class. Must be called as the very first method
 * before anything else is called inside this class.
 */
BEGINObjClassInit(rsconf, 1, OBJ_IS_CORE_MODULE) /* class, version */
	/* request objects we use */
	CHKiRet(objUse(ruleset, CORE_COMPONENT));
	CHKiRet(objUse(module, CORE_COMPONENT));
	CHKiRet(objUse(conf, CORE_COMPONENT));
	CHKiRet(objUse(errmsg, CORE_COMPONENT));
	CHKiRet(objUse(glbl, CORE_COMPONENT));

	/* now set our own handlers */
	OBJSetMethodHandler(objMethod_DEBUGPRINT, rsconfDebugPrint);
	OBJSetMethodHandler(objMethod_CONSTRUCTION_FINALIZER, rsconfConstructFinalize);
ENDObjClassInit(rsconf)


/* De-initialize the rsconf class.
 */
BEGINObjClassExit(rsconf, OBJ_IS_CORE_MODULE) /* class, version */
	objRelease(ruleset, CORE_COMPONENT);
	objRelease(module, CORE_COMPONENT);
	objRelease(conf, CORE_COMPONENT);
	objRelease(errmsg, CORE_COMPONENT);
	objRelease(glbl, CORE_COMPONENT);
ENDObjClassExit(rsconf)

/* vi:set ai:
 */
