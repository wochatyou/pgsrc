/*-------------------------------------------------------------------------
 *
 * walreceiver.c
 *
 * The WAL receiver process (walreceiver) is new as of Postgres 9.0. It
 * is the process in the standby server that takes charge of receiving
 * XLOG records from a primary server during streaming replication.
 *
 * When the startup process determines that it's time to start streaming,
 * it instructs postmaster to start walreceiver. Walreceiver first connects
 * to the primary server (it will be served by a walsender process
 * in the primary server), and then keeps receiving XLOG records and
 * writing them to the disk as long as the connection is alive. As XLOG
 * records are received and flushed to disk, it updates the
 * WalRcv->flushedUpto variable in shared memory, to inform the startup
 * process of how far it can proceed with XLOG replay.
 *
 * A WAL receiver cannot directly load GUC parameters used when establishing
 * its connection to the primary. Instead it relies on parameter values
 * that are passed down by the startup process when streaming is requested.
 * This applies, for example, to the replication slot and the connection
 * string to be used for the connection with the primary.
 *
 * If the primary server ends streaming, but doesn't disconnect, walreceiver
 * goes into "waiting" mode, and waits for the startup process to give new
 * instructions. The startup process will treat that the same as
 * disconnection, and will rescan the archive/pg_wal directory. But when the
 * startup process wants to try streaming replication again, it will just
 * nudge the existing walreceiver process that's waiting, instead of launching
 * a new one.
 *
 * Normal termination is by SIGTERM, which instructs the walreceiver to
 * exit(0). Emergency termination is by SIGQUIT; like any postmaster child
 * process, the walreceiver will simply abort and exit on SIGQUIT. A close
 * of the connection and a FATAL error are treated not as a crash but as
 * normal operation.
 *
 * This file contains the server-facing parts of walreceiver. The libpq-
 * specific parts are in the libpqwalreceiver module. It's loaded
 * dynamically to avoid linking the server with libpq.
 *
 * Portions Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/replication/walreceiver.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/htup_details.h"
#include "access/timeline.h"
#include "access/transam.h"
#include "access/xlog_internal.h"
#include "access/xlogarchive.h"
#include "access/xlogrecovery.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_type.h"
#include "common/ip.h"
#include "funcapi.h"
#include "libpq/pqformat.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/interrupt.h"
#include "replication/walreceiver.h"
#include "replication/walsender.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/pg_lsn.h"
#include "utils/ps_status.h"
#include "utils/resowner.h"
#include "utils/timestamp.h"


/*
 * GUC variables.  (Other variables that affect walreceiver are in xlog.c
 * because they're passed down from the startup process, for better
 * synchronization.)
 */
int			wal_receiver_status_interval;
int			wal_receiver_timeout;
bool		hot_standby_feedback;

/* libpqwalreceiver connection */
static WalReceiverConn *wrconn = NULL;
WalReceiverFunctionsType *WalReceiverFunctions = NULL;

/*
 * These variables are used similarly to openLogFile/SegNo,
 * but for walreceiver to write the XLOG. recvFileTLI is the TimeLineID
 * corresponding the filename of recvFile.
 */
static int	recvFile = -1;
static TimeLineID recvFileTLI = 0;
static XLogSegNo recvSegNo = 0;

/*
 * LogstreamResult indicates the byte positions that we have already
 * written/fsynced.
 */
static struct // 分为写的位置和刷新的位置
{
	XLogRecPtr	Write;			/* last byte + 1 written out in the standby */
	XLogRecPtr	Flush;			/* last byte + 1 flushed in the standby */
}			LogstreamResult;

/*
 * Reasons to wake up and perform periodic tasks.
 */
typedef enum WalRcvWakeupReason // wr(walreceiver)进程醒来的原因
{
	WALRCV_WAKEUP_TERMINATE,
	WALRCV_WAKEUP_PING,
	WALRCV_WAKEUP_REPLY,
	WALRCV_WAKEUP_HSFEEDBACK
#define NUM_WALRCV_WAKEUPS (WALRCV_WAKEUP_HSFEEDBACK + 1)
} WalRcvWakeupReason;

/*
 * Wake up times for periodic tasks.
 */
static TimestampTz wakeup[NUM_WALRCV_WAKEUPS]; // 这是一个数组，里面是醒来的原因，每一个占用一个槽。每个元素的值是时间

static StringInfoData reply_message;
static StringInfoData incoming_message;

/* Prototypes for private functions */
static void WalRcvFetchTimeLineHistoryFiles(TimeLineID first, TimeLineID last);
static void WalRcvWaitForStartPosition(XLogRecPtr *startpoint, TimeLineID *startpointTLI);
static void WalRcvDie(int code, Datum arg);
static void XLogWalRcvProcessMsg(unsigned char type, char *buf, Size len,
								 TimeLineID tli);
static void XLogWalRcvWrite(char *buf, Size nbytes, XLogRecPtr recptr,
							TimeLineID tli);
static void XLogWalRcvFlush(bool dying, TimeLineID tli);
static void XLogWalRcvClose(XLogRecPtr recptr, TimeLineID tli);
static void XLogWalRcvSendReply(bool force, bool requestReply);
static void XLogWalRcvSendHSFeedback(bool immed);
static void ProcessWalSndrMessage(XLogRecPtr walEnd, TimestampTz sendTime);
static void WalRcvComputeNextWakeup(WalRcvWakeupReason reason, TimestampTz now);

/*
 * Process any interrupts the walreceiver process may have received.
 * This should be called any time the process's latch has become set.
 *
 * Currently, only SIGTERM is of interest.  We can't just exit(1) within the
 * SIGTERM signal handler, because the signal might arrive in the middle of
 * some critical operation, like while we're holding a spinlock.  Instead, the
 * signal handler sets a flag variable as well as setting the process's latch.
 * We must check the flag (by calling ProcessWalRcvInterrupts) anytime the
 * latch has become set.  Operations that could block for a long time, such as
 * reading from a remote server, must pay attention to the latch too; see
 * libpqrcv_PQgetResult for example.
 */
void
ProcessWalRcvInterrupts(void)
{
	/*
	 * Although walreceiver interrupt handling doesn't use the same scheme as
	 * regular backends, call CHECK_FOR_INTERRUPTS() to make sure we receive
	 * any incoming signals on Win32, and also to make sure we process any
	 * barrier events.
	 */
	CHECK_FOR_INTERRUPTS();

	if (ShutdownRequestPending)
	{
		ereport(FATAL,
				(errcode(ERRCODE_ADMIN_SHUTDOWN),
				 errmsg("terminating walreceiver process due to administrator command")));
	}
}


/* Main entry point for walreceiver process */
void
WalReceiverMain(void) // wr进程的主入口函数
{
	char		conninfo[MAXCONNINFO];
	char	   *tmp_conninfo;
	char		slotname[NAMEDATALEN];
	bool		is_temp_slot;
	XLogRecPtr	startpoint;
	TimeLineID	startpointTLI;
	TimeLineID	primaryTLI;
	bool		first_stream;
	WalRcvData *walrcv = WalRcv; // 共享内存中的主要数据结构
	TimestampTz now;
	char	   *err;
	char	   *sender_host = NULL;
	int			sender_port = 0;

	/*
	 * WalRcv should be set up already (if we are a backend, we inherit this
	 * by fork() or EXEC_BACKEND mechanism from the postmaster).
	 */
	Assert(walrcv != NULL); // 共享内存中的内容已经设置好了

	/*
	 * Mark walreceiver as running in shared memory.
	 *
	 * Do this as early as possible, so that if we fail later on, we'll set
	 * state to STOPPED. If we die before this, the startup process will keep
	 * waiting for us to start up, until it times out.
	 */
	SpinLockAcquire(&walrcv->mutex);
	Assert(walrcv->pid == 0); // 现在是walreceiver启动阶段，共享内存中的pid肯定是0
	switch (walrcv->walRcvState)
	{
		case WALRCV_STOPPING:
			/* If we've already been requested to stop, don't start up. */
			walrcv->walRcvState = WALRCV_STOPPED;
			/* fall through */

		case WALRCV_STOPPED:
			SpinLockRelease(&walrcv->mutex);
			ConditionVariableBroadcast(&walrcv->walRcvStoppedCV);
			proc_exit(1);  // 退出本进程
			break;

		case WALRCV_STARTING:  //startup进程呼叫启动本进程时，会设置状态为WALRCV_STARTING
			/* The usual case */
			break;

		case WALRCV_WAITING:
		case WALRCV_STREAMING:
		case WALRCV_RESTARTING:
		default:
			/* Shouldn't happen */
			SpinLockRelease(&walrcv->mutex);
			elog(PANIC, "walreceiver still running according to shared memory state");
	}
	/* Advertise our PID so that the startup process can kill us */
	walrcv->pid = MyProcPid; // 登记一下本进程的进程号
	walrcv->walRcvState = WALRCV_STREAMING; // 把状态设置为WALRCV_STREAMING

	/* Fetch information required to start streaming */
	walrcv->ready_to_display = false;
	strlcpy(conninfo, (char *) walrcv->conninfo, MAXCONNINFO); // 这些连接信息是startup进程在请求主进程启动walreceiver进程之前就填好了
	strlcpy(slotname, (char *) walrcv->slotname, NAMEDATALEN);
	is_temp_slot = walrcv->is_temp_slot;
	startpoint = walrcv->receiveStart;  // 向主库索取WAL记录的起点
	startpointTLI = walrcv->receiveStartTLI; // 向主库索取WAL记录的时间线，这些信息startup进程应该已经设置好了

	/*
	 * At most one of is_temp_slot and slotname can be set; otherwise,
	 * RequestXLogStreaming messed up.
	 */
	Assert(!is_temp_slot || (slotname[0] == '\0')); // 两个条件只能设置一个，如果两个都设置了，说明出错了

	/* Initialise to a sanish value */
	now = GetCurrentTimestamp();
	walrcv->lastMsgSendTime =
		walrcv->lastMsgReceiptTime = walrcv->latestWalEndTime = now; // 更新一下时间

	/* Report the latch to use to awaken this process */
	walrcv->latch = &MyProc->procLatch;

	SpinLockRelease(&walrcv->mutex);

	pg_atomic_write_u64(&WalRcv->writtenUpto, 0); // 原子性地往64位共享内存写入0

	/* Arrange to clean up at walreceiver exit */
	on_shmem_exit(WalRcvDie, PointerGetDatum(&startpointTLI));

	/* Properly accept or ignore signals the postmaster might send us */
	pqsignal(SIGHUP, SignalHandlerForConfigReload); /* set flag to read config
													 * file */
	pqsignal(SIGINT, SIG_IGN);
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest); /* request shutdown */
	/* SIGQUIT handler was already set up by InitPostmasterChild */
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, SIG_IGN);

	/* Reset some signals that are accepted by postmaster but not here */
	pqsignal(SIGCHLD, SIG_DFL);

	/* Load the libpq-specific functions */
	load_file("libpqwalreceiver", false); // 为了避免客户端的链接库代码连接到postgres主程序中，这里采用了动态加载的方法，因为只有walreceiver进程需要使用它
	// load_file会自动执行动态库中的_PG_init()函数
	// 如果加载成功，动态库在初始化函数中会设置WalReceiverFunctions，一大堆的回调函数会被正确的挂载上
	if (WalReceiverFunctions == NULL)
		elog(ERROR, "libpqwalreceiver didn't initialize correctly");

	/* Unblock signals (they were blocked when the postmaster forked us) */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/* Establish the connection to the primary for XLOG streaming */
	wrconn = walrcv_connect(conninfo, false, false,
							cluster_name[0] ? cluster_name : "walreceiver",
							&err);
	if (!wrconn) // 如果连接不成功，就报错退出了
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not connect to the primary server: %s", err)));

	/*
	 * Save user-visible connection string.  This clobbers the original
	 * conninfo, for security. Also save host and port of the sender server
	 * this walreceiver is connected to.
	 */
	tmp_conninfo = walrcv_get_conninfo(wrconn); // 这些信息可以通过pg_stat_wal_receiver这个系统视图查询到。这是实际上就是执行回调函数
	walrcv_get_senderinfo(wrconn, &sender_host, &sender_port);
	SpinLockAcquire(&walrcv->mutex);
	memset(walrcv->conninfo, 0, MAXCONNINFO);
	if (tmp_conninfo)
		strlcpy((char *) walrcv->conninfo, tmp_conninfo, MAXCONNINFO);

	memset(walrcv->sender_host, 0, NI_MAXHOST);
	if (sender_host)
		strlcpy((char *) walrcv->sender_host, sender_host, NI_MAXHOST);

	walrcv->sender_port = sender_port;
	walrcv->ready_to_display = true;
	SpinLockRelease(&walrcv->mutex);

	if (tmp_conninfo)
		pfree(tmp_conninfo);

	if (sender_host)
		pfree(sender_host);

	first_stream = true;  // 第一次进行stream
	for (;;)  // 无限循环
	{
		char	   *primary_sysid;
		char		standby_sysid[32];
		WalRcvStreamOptions options;

		/*
		 * Check that we're connected to a valid server using the
		 * IDENTIFY_SYSTEM replication command.
		 */
		primary_sysid = walrcv_identify_system(wrconn, &primaryTLI);  // 向主库索要系统标识符

		snprintf(standby_sysid, sizeof(standby_sysid), UINT64_FORMAT,
				 GetSystemIdentifier()); // GetSystemIdentifier()就是直接读取控制文件中的系统标识符
		if (strcmp(primary_sysid, standby_sysid) != 0) // 如果主库的系统标识符和本库控制文件中的系统标识符不一致，则报错退出
		{
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("database system identifier differs between the primary and standby"),
					 errdetail("The primary's identifier is %s, the standby's identifier is %s.",
							   primary_sysid, standby_sysid)));
		}

		/*
		 * Confirm that the current timeline of the primary is the same or
		 * ahead of ours.
		 */
		if (primaryTLI < startpointTLI) // 备库的时间线不可能大于主库的时间线
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("highest timeline %u of the primary is behind recovery timeline %u",
							primaryTLI, startpointTLI)));

		/*
		 * Get any missing history files. We do this always, even when we're
		 * not interested in that timeline, so that if we're promoted to
		 * become the primary later on, we don't select the same timeline that
		 * was already used in the current primary. This isn't bullet-proof -
		 * you'll need some external software to manage your cluster if you
		 * need to ensure that a unique timeline id is chosen in every case,
		 * but let's avoid the confusion of timeline id collisions where we
		 * can.
		 */
		WalRcvFetchTimeLineHistoryFiles(startpointTLI, primaryTLI); // 获取备库时间线和主库时间线之间的时间线切换历史

		/*
		 * Create temporary replication slot if requested, and update slot
		 * name in shared memory.  (Note the slot name cannot already be set
		 * in this case.)
		 */
		if (is_temp_slot)
		{
			snprintf(slotname, sizeof(slotname),
					 "pg_walreceiver_%lld",
					 (long long int) walrcv_get_backend_pid(wrconn)); // 根据一定的规则产生一个临时复制槽的名字

			walrcv_create_slot(wrconn, slotname, true, false, 0, NULL); // 创建复制槽

			SpinLockAcquire(&walrcv->mutex);
			strlcpy(walrcv->slotname, slotname, NAMEDATALEN); // 把复制槽的名字放在共享内存中
			SpinLockRelease(&walrcv->mutex);
		}

		/*
		 * Start streaming.
		 *
		 * We'll try to start at the requested starting point and timeline,
		 * even if it's different from the server's latest timeline. In case
		 * we've already reached the end of the old timeline, the server will
		 * finish the streaming immediately, and we will go back to await
		 * orders from the startup process. If recovery_target_timeline is
		 * 'latest', the startup process will scan pg_wal and find the new
		 * history file, bump recovery target timeline, and ask us to restart
		 * on the new timeline.
		 */
		options.logical = false; // 是物理复制，不是逻辑复制
		options.startpoint = startpoint; // 向主库索取从这一个LSN开始的WAL记录
		options.slotname = slotname[0] != '\0' ? slotname : NULL;
		options.proto.physical.startpointTLI = startpointTLI; //开始的时间线
		if (walrcv_startstreaming(wrconn, &options))
		{
			if (first_stream) //如果是第一次，就打印一条日志，告诉用户我是从哪个时间线，哪个LSN开始起步的
				ereport(LOG,
						(errmsg("started streaming WAL from primary at %X/%X on timeline %u",
								LSN_FORMAT_ARGS(startpoint), startpointTLI)));
			else
				ereport(LOG,
						(errmsg("restarted WAL streaming at %X/%X on timeline %u",
								LSN_FORMAT_ARGS(startpoint), startpointTLI)));
			first_stream = false; // 以后就不是第一次了

			/* Initialize LogstreamResult and buffers for processing messages */
			LogstreamResult.Write = LogstreamResult.Flush = GetXLogReplayRecPtr(NULL); // 获得startup进程最后的apply的LSN，时间线我们不关心，已经有了
			initStringInfo(&reply_message);
			initStringInfo(&incoming_message);

			/* Initialize nap wakeup times. */
			now = GetCurrentTimestamp();
			for (int i = 0; i < NUM_WALRCV_WAKEUPS; ++i) // 为每一种原因设置超时时间
				WalRcvComputeNextWakeup(i, now);

			/* Send initial reply/feedback messages. */
			XLogWalRcvSendReply(true, false);
			XLogWalRcvSendHSFeedback(true);

			/* Loop until end-of-streaming or error */
			for (;;)
			{
				char	   *buf;
				int			len;
				bool		endofwal = false;
				pgsocket	wait_fd = PGINVALID_SOCKET;
				int			rc;
				TimestampTz nextWakeup;
				long		nap;

				/*
				 * Exit walreceiver if we're not in recovery. This should not
				 * happen, but cross-check the status here.
				 */
				if (!RecoveryInProgress()) // 判断数据库是否还处于恢复状态。只有处于恢复状态，walreceiver的工作才有意义
					ereport(FATAL,
							(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							 errmsg("cannot continue WAL streaming, recovery has already ended")));

				/* Process any requests or signals received recently */
				ProcessWalRcvInterrupts(); // 处理最近收到的请求或者信号

				if (ConfigReloadPending) // 如果要求重新加载参数，就加载一下
				{
					ConfigReloadPending = false;
					ProcessConfigFile(PGC_SIGHUP);
					/* recompute wakeup times */
					now = GetCurrentTimestamp();
					for (int i = 0; i < NUM_WALRCV_WAKEUPS; ++i) // 可能超时时间参数修改了，要重新设置一下各种原因的超时时间
						WalRcvComputeNextWakeup(i, now);
					XLogWalRcvSendHSFeedback(true);
				}

				/* See if we can read data immediately */
				len = walrcv_receive(wrconn, &buf, &wait_fd); // 物理复制和逻辑复制都使用这个函数从主库获得数据
				if (len != 0) // 拿到了数据了
				{
					/*
					 * Process the received data, and any subsequent data we
					 * can read without blocking.
					 */
					for (;;)
					{
						if (len > 0)
						{
							/*
							 * Something was received from primary, so adjust
							 * the ping and terminate wakeup times.
							 */
							now = GetCurrentTimestamp(); // 从主库获得了一些信息，重新调整下面两个原因的超时时间
							WalRcvComputeNextWakeup(WALRCV_WAKEUP_TERMINATE,
													now);
							WalRcvComputeNextWakeup(WALRCV_WAKEUP_PING, now);
							XLogWalRcvProcessMsg(buf[0], &buf[1], len - 1,
												 startpointTLI); // 处理来自主库的消息包
						}
						else if (len == 0)
							break;
						else if (len < 0)
						{
							ereport(LOG,
									(errmsg("replication terminated by primary server"),
									 errdetail("End of WAL reached on timeline %u at %X/%X.",
											   startpointTLI,
											   LSN_FORMAT_ARGS(LogstreamResult.Write))));
							endofwal = true;
							break;
						}
						len = walrcv_receive(wrconn, &buf, &wait_fd); // 继续接收来自主库的消息包
					}

					/* Let the primary know that we received some data. */
					XLogWalRcvSendReply(false, false); // 给主库反馈一些信息

					/*
					 * If we've written some records, flush them to disk and
					 * let the startup process and primary server know about
					 * them.
					 */
					XLogWalRcvFlush(false, startpointTLI); // walreceiver进程把WAL记录写入磁盘。它和startup之间的通讯是通过磁盘进行的。
				}

				/* Check if we need to exit the streaming loop. */
				if (endofwal)
					break;

				/* Find the soonest wakeup time, to limit our nap. */
				nextWakeup = TIMESTAMP_INFINITY;
				for (int i = 0; i < NUM_WALRCV_WAKEUPS; ++i)
					nextWakeup = Min(wakeup[i], nextWakeup); // 扫描原因数组，拿到最小的超时时间

				/* Calculate the nap time, clamping as necessary. */
				now = GetCurrentTimestamp();
				nap = TimestampDifferenceMilliseconds(now, nextWakeup); // 计算一下我可以睡眠多长时间

				/*
				 * Ideally we would reuse a WaitEventSet object repeatedly
				 * here to avoid the overheads of WaitLatchOrSocket on epoll
				 * systems, but we can't be sure that libpq (or any other
				 * walreceiver implementation) has the same socket (even if
				 * the fd is the same number, it may have been closed and
				 * reopened since the last time).  In future, if there is a
				 * function for removing sockets from WaitEventSet, then we
				 * could add and remove just the socket each time, potentially
				 * avoiding some system calls.
				 */
				Assert(wait_fd != PGINVALID_SOCKET);
				rc = WaitLatchOrSocket(MyLatch,
									   WL_EXIT_ON_PM_DEATH | WL_SOCKET_READABLE |
									   WL_TIMEOUT | WL_LATCH_SET,
									   wait_fd,
									   nap,
									   WAIT_EVENT_WAL_RECEIVER_MAIN);
				if (rc & WL_LATCH_SET)
				{
					ResetLatch(MyLatch);
					ProcessWalRcvInterrupts();

					if (walrcv->force_reply)
					{
						/*
						 * The recovery process has asked us to send apply
						 * feedback now.  Make sure the flag is really set to
						 * false in shared memory before sending the reply, so
						 * we don't miss a new request for a reply.
						 */
						walrcv->force_reply = false;
						pg_memory_barrier();
						XLogWalRcvSendReply(true, false);
					}
				}
				if (rc & WL_TIMEOUT)
				{
					/*
					 * We didn't receive anything new. If we haven't heard
					 * anything from the server for more than
					 * wal_receiver_timeout / 2, ping the server. Also, if
					 * it's been longer than wal_receiver_status_interval
					 * since the last update we sent, send a status update to
					 * the primary anyway, to report any progress in applying
					 * WAL.
					 */
					bool		requestReply = false;

					/*
					 * Check if time since last receive from primary has
					 * reached the configured limit.
					 */
					now = GetCurrentTimestamp();
					if (now >= wakeup[WALRCV_WAKEUP_TERMINATE])
						ereport(ERROR,
								(errcode(ERRCODE_CONNECTION_FAILURE),
								 errmsg("terminating walreceiver due to timeout")));

					/*
					 * If we didn't receive anything new for half of receiver
					 * replication timeout, then ping the server.
					 */
					if (now >= wakeup[WALRCV_WAKEUP_PING])
					{
						requestReply = true;
						wakeup[WALRCV_WAKEUP_PING] = TIMESTAMP_INFINITY;
					}

					XLogWalRcvSendReply(requestReply, requestReply);
					XLogWalRcvSendHSFeedback(false);
				}
			}

			/*
			 * The backend finished streaming. Exit streaming COPY-mode from
			 * our side, too.
			 */
			walrcv_endstreaming(wrconn, &primaryTLI);

			/*
			 * If the server had switched to a new timeline that we didn't
			 * know about when we began streaming, fetch its timeline history
			 * file now.
			 */
			WalRcvFetchTimeLineHistoryFiles(startpointTLI, primaryTLI);
		}
		else
			ereport(LOG,
					(errmsg("primary server contains no more WAL on requested timeline %u",
							startpointTLI)));

		/*
		 * End of WAL reached on the requested timeline. Close the last
		 * segment, and await for new orders from the startup process.
		 */
		if (recvFile >= 0)
		{
			char		xlogfname[MAXFNAMELEN];

			XLogWalRcvFlush(false, startpointTLI);
			XLogFileName(xlogfname, recvFileTLI, recvSegNo, wal_segment_size);
			if (close(recvFile) != 0)
				ereport(PANIC,
						(errcode_for_file_access(),
						 errmsg("could not close WAL segment %s: %m",
								xlogfname)));

			/*
			 * Create .done file forcibly to prevent the streamed segment from
			 * being archived later.
			 */
			if (XLogArchiveMode != ARCHIVE_MODE_ALWAYS) // archive_mode参数为always
				XLogArchiveForceDone(xlogfname);
			else
				XLogArchiveNotify(xlogfname);
		}
		recvFile = -1;

		elog(DEBUG1, "walreceiver ended streaming and awaits new instructions");
		WalRcvWaitForStartPosition(&startpoint, &startpointTLI);
	}
	/* not reached */
}

/*
 * Wait for startup process to set receiveStart and receiveStartTLI.
 */
static void
WalRcvWaitForStartPosition(XLogRecPtr *startpoint, TimeLineID *startpointTLI)
{
	WalRcvData *walrcv = WalRcv;
	int			state;

	SpinLockAcquire(&walrcv->mutex);
	state = walrcv->walRcvState;
	if (state != WALRCV_STREAMING)
	{
		SpinLockRelease(&walrcv->mutex);
		if (state == WALRCV_STOPPING)
			proc_exit(0);
		else
			elog(FATAL, "unexpected walreceiver state");
	}
	walrcv->walRcvState = WALRCV_WAITING;
	walrcv->receiveStart = InvalidXLogRecPtr;
	walrcv->receiveStartTLI = 0;
	SpinLockRelease(&walrcv->mutex);

	set_ps_display("idle");

	/*
	 * nudge startup process to notice that we've stopped streaming and are
	 * now waiting for instructions.
	 */
	WakeupRecovery();
	for (;;)
	{
		ResetLatch(MyLatch);

		ProcessWalRcvInterrupts();

		SpinLockAcquire(&walrcv->mutex);
		Assert(walrcv->walRcvState == WALRCV_RESTARTING ||
			   walrcv->walRcvState == WALRCV_WAITING ||
			   walrcv->walRcvState == WALRCV_STOPPING);
		if (walrcv->walRcvState == WALRCV_RESTARTING)
		{
			/*
			 * No need to handle changes in primary_conninfo or
			 * primary_slot_name here. Startup process will signal us to
			 * terminate in case those change.
			 */
			*startpoint = walrcv->receiveStart;
			*startpointTLI = walrcv->receiveStartTLI;
			walrcv->walRcvState = WALRCV_STREAMING;
			SpinLockRelease(&walrcv->mutex);
			break;
		}
		if (walrcv->walRcvState == WALRCV_STOPPING)
		{
			/*
			 * We should've received SIGTERM if the startup process wants us
			 * to die, but might as well check it here too.
			 */
			SpinLockRelease(&walrcv->mutex);
			exit(1);
		}
		SpinLockRelease(&walrcv->mutex);

		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, 0,
						 WAIT_EVENT_WAL_RECEIVER_WAIT_START);
	}

	if (update_process_title)
	{
		char		activitymsg[50];

		snprintf(activitymsg, sizeof(activitymsg), "restarting at %X/%X",
				 LSN_FORMAT_ARGS(*startpoint));
		set_ps_display(activitymsg);
	}
}

/*
 * Fetch any missing timeline history files between 'first' and 'last'
 * (inclusive) from the server.
 */
static void
WalRcvFetchTimeLineHistoryFiles(TimeLineID first, TimeLineID last)
{
	TimeLineID	tli;

	for (tli = first; tli <= last; tli++)
	{
		/* there's no history file for timeline 1 */
		if (tli != 1 && !existsTimeLineHistory(tli))
		{
			char	   *fname;
			char	   *content;
			int			len;
			char		expectedfname[MAXFNAMELEN];

			ereport(LOG,
					(errmsg("fetching timeline history file for timeline %u from primary server",
							tli)));

			walrcv_readtimelinehistoryfile(wrconn, tli, &fname, &content, &len);

			/*
			 * Check that the filename on the primary matches what we
			 * calculated ourselves. This is just a sanity check, it should
			 * always match.
			 */
			TLHistoryFileName(expectedfname, tli);
			if (strcmp(fname, expectedfname) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg_internal("primary reported unexpected file name for timeline history file of timeline %u",
										 tli)));

			/*
			 * Write the file to pg_wal.
			 */
			writeTimeLineHistoryFile(tli, content, len);

			/*
			 * Mark the streamed history file as ready for archiving if
			 * archive_mode is always.
			 */
			if (XLogArchiveMode != ARCHIVE_MODE_ALWAYS)
				XLogArchiveForceDone(fname);
			else
				XLogArchiveNotify(fname);

			pfree(fname);
			pfree(content);
		}
	}
}

/*
 * Mark us as STOPPED in shared memory at exit.
 */
static void
WalRcvDie(int code, Datum arg)
{
	WalRcvData *walrcv = WalRcv;
	TimeLineID *startpointTLI_p = (TimeLineID *) DatumGetPointer(arg);

	Assert(*startpointTLI_p != 0);

	/* Ensure that all WAL records received are flushed to disk */
	XLogWalRcvFlush(true, *startpointTLI_p);

	/* Mark ourselves inactive in shared memory */
	SpinLockAcquire(&walrcv->mutex);
	Assert(walrcv->walRcvState == WALRCV_STREAMING ||
		   walrcv->walRcvState == WALRCV_RESTARTING ||
		   walrcv->walRcvState == WALRCV_STARTING ||
		   walrcv->walRcvState == WALRCV_WAITING ||
		   walrcv->walRcvState == WALRCV_STOPPING);
	Assert(walrcv->pid == MyProcPid);
	walrcv->walRcvState = WALRCV_STOPPED;
	walrcv->pid = 0;
	walrcv->ready_to_display = false;
	walrcv->latch = NULL;
	SpinLockRelease(&walrcv->mutex);

	ConditionVariableBroadcast(&walrcv->walRcvStoppedCV);

	/* Terminate the connection gracefully. */
	if (wrconn != NULL)
		walrcv_disconnect(wrconn);

	/* Wake up the startup process to notice promptly that we're gone */
	WakeupRecovery();
}

/*
 * Accept the message from XLOG stream, and process it.
 */
static void // 处理来自主库的消息包，type是消息包的第一个字节，表示消息的类型。buf是消息包的第二个字节开始的
XLogWalRcvProcessMsg(unsigned char type, char *buf, Size len, TimeLineID tli)
{
	int			hdrlen;
	XLogRecPtr	dataStart;
	XLogRecPtr	walEnd;
	TimestampTz sendTime;
	bool		replyRequested;

	resetStringInfo(&incoming_message); // 重新置位字符串

	switch (type)
	{
		case 'w':				/* WAL records */ // w是真正的数据包
			{
				/* copy message to StringInfo */
				hdrlen = sizeof(int64) + sizeof(int64) + sizeof(int64); // 消息头部是24个字节
				if (len < hdrlen)
					ereport(ERROR,
							(errcode(ERRCODE_PROTOCOL_VIOLATION),
							 errmsg_internal("invalid WAL message received from primary")));
				appendBinaryStringInfo(&incoming_message, buf, hdrlen);

				/* read the fields */
				dataStart = pq_getmsgint64(&incoming_message);
				walEnd = pq_getmsgint64(&incoming_message);
				sendTime = pq_getmsgint64(&incoming_message);
				ProcessWalSndrMessage(walEnd, sendTime); // 更新一下pg_stat_wal_receiver里面的信息

				buf += hdrlen;
				len -= hdrlen;
				XLogWalRcvWrite(buf, len, dataStart, tli); // 写入本地磁盘的WAL文件中
				break;
			}
		case 'k':				/* Keepalive */   // k表示keepalive，心跳功能
			{
				/* copy message to StringInfo */
				hdrlen = sizeof(int64) + sizeof(int64) + sizeof(char); // 8 + 8 + 1共计17个字节，消息头部是17个字节
				if (len != hdrlen)
					ereport(ERROR,
							(errcode(ERRCODE_PROTOCOL_VIOLATION),
							 errmsg_internal("invalid keepalive message received from primary")));
				appendBinaryStringInfo(&incoming_message, buf, hdrlen);

				/* read the fields */
				walEnd = pq_getmsgint64(&incoming_message); // 这是LSN
				sendTime = pq_getmsgint64(&incoming_message); // 这是发送的时间
				replyRequested = pq_getmsgbyte(&incoming_message); // 是否需要回复给主库

				ProcessWalSndrMessage(walEnd, sendTime); //更新一下pg_stat_wal_receiver里面的信息

				/* If the primary requested a reply, send one immediately */
				if (replyRequested) // 如果主库需要回复，就发一个回复给它
					XLogWalRcvSendReply(true, false);
				break;
			}
		default: // 如果收到的主库的消息包不是w或者k，说明出现异常，直接退出本进程了
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg_internal("invalid replication message type %d",
									 type)));
	}
}

/*
 * Write XLOG data to disk.
 */
static void // 把WAL记录写入到磁盘上
XLogWalRcvWrite(char *buf, Size nbytes, XLogRecPtr recptr, TimeLineID tli)
{
	int			startoff;
	int			byteswritten;

	Assert(tli != 0);

	while (nbytes > 0)
	{
		int			segbytes;

		/* Close the current segment if it's completed */
		if (recvFile >= 0 && !XLByteInSeg(recptr, recvSegNo, wal_segment_size)) // WAL文件打开了，但是LSN不在这个文件的范围内
			XLogWalRcvClose(recptr, tli); // 写满了本WAL文件，要写下一个WAL文件

		if (recvFile < 0)
		{
			/* Create/use new log file */
			XLByteToSeg(recptr, recvSegNo, wal_segment_size);
			recvFile = XLogFileInit(recvSegNo, tli); // 打开recptr所在的WAL文件
			recvFileTLI = tli;
		}

		/* Calculate the start offset of the received logs */
		startoff = XLogSegmentOffset(recptr, wal_segment_size); // 获得recptr在这个WAL文件中的偏移量

		if (startoff + nbytes > wal_segment_size) // 如果消息包超过了WAL文件的尾部
			segbytes = wal_segment_size - startoff;
		else
			segbytes = nbytes; // 最后segbytes表明从recptr开始要往本WAL文件中写入多少个字节

		/* OK to write the logs */
		errno = 0;

		byteswritten = pg_pwrite(recvFile, buf, segbytes, (off_t) startoff); // 往WAL文件中写入字节
		if (byteswritten <= 0) // 如果写入出错，就报错退出本进程
		{
			char		xlogfname[MAXFNAMELEN];
			int			save_errno;

			/* if write didn't set errno, assume no disk space */
			if (errno == 0)
				errno = ENOSPC;

			save_errno = errno;
			XLogFileName(xlogfname, recvFileTLI, recvSegNo, wal_segment_size);
			errno = save_errno;
			ereport(PANIC,
					(errcode_for_file_access(),
					 errmsg("could not write to WAL segment %s "
							"at offset %u, length %lu: %m",
							xlogfname, startoff, (unsigned long) segbytes)));
		}

		/* Update state for write */
		recptr += byteswritten; //调整下一次写入的位置

		nbytes -= byteswritten;
		buf += byteswritten;

		LogstreamResult.Write = recptr; // 记录一下写入的位置
	}

	/* Update shared-memory status */
	pg_atomic_write_u64(&WalRcv->writtenUpto, LogstreamResult.Write);

	/*
	 * Close the current segment if it's fully written up in the last cycle of
	 * the loop, to create its archive notification file soon. Otherwise WAL
	 * archiving of the segment will be delayed until any data in the next
	 * segment is received and written.
	 */
	if (recvFile >= 0 && !XLByteInSeg(recptr, recvSegNo, wal_segment_size))
		XLogWalRcvClose(recptr, tli);
}

/*
 * Flush the log to disk.
 *
 * If we're in the midst of dying, it's unwise to do anything that might throw
 * an error, so we skip sending a reply in that case.
 */
static void // 把当前打开的文件同步到磁盘上
XLogWalRcvFlush(bool dying, TimeLineID tli)
{
	Assert(tli != 0);

	if (LogstreamResult.Flush < LogstreamResult.Write)
	{
		WalRcvData *walrcv = WalRcv;

		issue_xlog_fsync(recvFile, recvSegNo, tli); // 这里做的是真正的fync

		LogstreamResult.Flush = LogstreamResult.Write;

		/* Update shared-memory status */
		SpinLockAcquire(&walrcv->mutex);
		if (walrcv->flushedUpto < LogstreamResult.Flush)
		{
			walrcv->latestChunkStart = walrcv->flushedUpto;
			walrcv->flushedUpto = LogstreamResult.Flush;
			walrcv->receivedTLI = tli;
		}
		SpinLockRelease(&walrcv->mutex);

		/* Signal the startup process and walsender that new WAL has arrived */
		WakeupRecovery();
		if (AllowCascadeReplication())
			WalSndWakeup(true, false);

		/* Report XLOG streaming progress in PS display */
		if (update_process_title)
		{
			char		activitymsg[50];

			snprintf(activitymsg, sizeof(activitymsg), "streaming %X/%X",
					 LSN_FORMAT_ARGS(LogstreamResult.Write));
			set_ps_display(activitymsg);
		}

		/* Also let the primary know that we made some progress */
		if (!dying)
		{
			XLogWalRcvSendReply(false, false);
			XLogWalRcvSendHSFeedback(false);
		}
	}
}

/*
 * Close the current segment.
 *
 * Flush the segment to disk before closing it. Otherwise we have to
 * reopen and fsync it later.
 *
 * Create an archive notification file since the segment is known completed.
 */
static void
XLogWalRcvClose(XLogRecPtr recptr, TimeLineID tli)
{
	char		xlogfname[MAXFNAMELEN];

	Assert(recvFile >= 0 && !XLByteInSeg(recptr, recvSegNo, wal_segment_size));
	Assert(tli != 0);

	/*
	 * fsync() and close current file before we switch to next one. We would
	 * otherwise have to reopen this file to fsync it later
	 */
	XLogWalRcvFlush(false, tli); // 使用fync把当前已经打开的WAL文件刷新到磁盘上

	XLogFileName(xlogfname, recvFileTLI, recvSegNo, wal_segment_size);

	/*
	 * XLOG segment files will be re-read by recovery in startup process soon,
	 * so we don't advise the OS to release cache pages associated with the
	 * file like XLogFileClose() does.
	 */
	if (close(recvFile) != 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not close WAL segment %s: %m",
						xlogfname)));

	/*
	 * Create .done file forcibly to prevent the streamed segment from being
	 * archived later.
	 */
	if (XLogArchiveMode != ARCHIVE_MODE_ALWAYS)
		XLogArchiveForceDone(xlogfname);
	else
		XLogArchiveNotify(xlogfname);

	recvFile = -1;
}

/*
 * Send reply message to primary, indicating our current WAL locations, oldest
 * xmin and the current time.
 *
 * If 'force' is not set, the message is only sent if enough time has
 * passed since last status update to reach wal_receiver_status_interval.
 * If wal_receiver_status_interval is disabled altogether and 'force' is
 * false, this is a no-op.
 *
 * If 'requestReply' is true, requests the server to reply immediately upon
 * receiving this message. This is used for heartbeats, when approaching
 * wal_receiver_timeout.
 */
static void
XLogWalRcvSendReply(bool force, bool requestReply)
{
	static XLogRecPtr writePtr = 0;
	static XLogRecPtr flushPtr = 0;
	XLogRecPtr	applyPtr;
	TimestampTz now;

	/*
	 * If the user doesn't want status to be reported to the primary, be sure
	 * to exit before doing anything at all.
	 */
	if (!force && wal_receiver_status_interval <= 0)
		return;

	/* Get current timestamp. */
	now = GetCurrentTimestamp();

	/*
	 * We can compare the write and flush positions to the last message we
	 * sent without taking any lock, but the apply position requires a spin
	 * lock, so we don't check that unless something else has changed or 10
	 * seconds have passed.  This means that the apply WAL location will
	 * appear, from the primary's point of view, to lag slightly, but since
	 * this is only for reporting purposes and only on idle systems, that's
	 * probably OK.
	 */
	if (!force
		&& writePtr == LogstreamResult.Write
		&& flushPtr == LogstreamResult.Flush
		&& now < wakeup[WALRCV_WAKEUP_REPLY])
		return;

	/* Make sure we wake up when it's time to send another reply. */
	WalRcvComputeNextWakeup(WALRCV_WAKEUP_REPLY, now);

	/* Construct a new message */
	writePtr = LogstreamResult.Write;
	flushPtr = LogstreamResult.Flush;
	applyPtr = GetXLogReplayRecPtr(NULL);

	resetStringInfo(&reply_message);
	pq_sendbyte(&reply_message, 'r');
	pq_sendint64(&reply_message, writePtr);
	pq_sendint64(&reply_message, flushPtr);
	pq_sendint64(&reply_message, applyPtr);
	pq_sendint64(&reply_message, GetCurrentTimestamp());
	pq_sendbyte(&reply_message, requestReply ? 1 : 0);

	/* Send it */
	elog(DEBUG2, "sending write %X/%X flush %X/%X apply %X/%X%s",
		 LSN_FORMAT_ARGS(writePtr),
		 LSN_FORMAT_ARGS(flushPtr),
		 LSN_FORMAT_ARGS(applyPtr),
		 requestReply ? " (reply requested)" : "");

	walrcv_send(wrconn, reply_message.data, reply_message.len);
}

/*
 * Send hot standby feedback message to primary, plus the current time,
 * in case they don't have a watch.
 *
 * If the user disables feedback, send one final message to tell sender
 * to forget about the xmin on this standby. We also send this message
 * on first connect because a previous connection might have set xmin
 * on a replication slot. (If we're not using a slot it's harmless to
 * send a feedback message explicitly setting InvalidTransactionId).
 */
static void
XLogWalRcvSendHSFeedback(bool immed)
{
	TimestampTz now;
	FullTransactionId nextFullXid;
	TransactionId nextXid;
	uint32		xmin_epoch,
				catalog_xmin_epoch;
	TransactionId xmin,
				catalog_xmin;

	/* initially true so we always send at least one feedback message */
	static bool primary_has_standby_xmin = true;

	/*
	 * If the user doesn't want status to be reported to the primary, be sure
	 * to exit before doing anything at all.
	 */
	if ((wal_receiver_status_interval <= 0 || !hot_standby_feedback) &&
		!primary_has_standby_xmin)
		return;

	/* Get current timestamp. */
	now = GetCurrentTimestamp();

	/* Send feedback at most once per wal_receiver_status_interval. */
	if (!immed && now < wakeup[WALRCV_WAKEUP_HSFEEDBACK])
		return;

	/* Make sure we wake up when it's time to send feedback again. */
	WalRcvComputeNextWakeup(WALRCV_WAKEUP_HSFEEDBACK, now);

	/*
	 * If Hot Standby is not yet accepting connections there is nothing to
	 * send. Check this after the interval has expired to reduce number of
	 * calls.
	 *
	 * Bailing out here also ensures that we don't send feedback until we've
	 * read our own replication slot state, so we don't tell the primary to
	 * discard needed xmin or catalog_xmin from any slots that may exist on
	 * this replica.
	 */
	if (!HotStandbyActive())
		return;

	/*
	 * Make the expensive call to get the oldest xmin once we are certain
	 * everything else has been checked.
	 */
	if (hot_standby_feedback)
	{
		GetReplicationHorizons(&xmin, &catalog_xmin);
	}
	else
	{
		xmin = InvalidTransactionId;
		catalog_xmin = InvalidTransactionId;
	}

	/*
	 * Get epoch and adjust if nextXid and oldestXmin are different sides of
	 * the epoch boundary.
	 */
	nextFullXid = ReadNextFullTransactionId();
	nextXid = XidFromFullTransactionId(nextFullXid);
	xmin_epoch = EpochFromFullTransactionId(nextFullXid);
	catalog_xmin_epoch = xmin_epoch;
	if (nextXid < xmin)
		xmin_epoch--;
	if (nextXid < catalog_xmin)
		catalog_xmin_epoch--;

	elog(DEBUG2, "sending hot standby feedback xmin %u epoch %u catalog_xmin %u catalog_xmin_epoch %u",
		 xmin, xmin_epoch, catalog_xmin, catalog_xmin_epoch);

	/* Construct the message and send it. */
	resetStringInfo(&reply_message);
	pq_sendbyte(&reply_message, 'h');
	pq_sendint64(&reply_message, GetCurrentTimestamp());
	pq_sendint32(&reply_message, xmin);
	pq_sendint32(&reply_message, xmin_epoch);
	pq_sendint32(&reply_message, catalog_xmin);
	pq_sendint32(&reply_message, catalog_xmin_epoch);
	walrcv_send(wrconn, reply_message.data, reply_message.len);
	if (TransactionIdIsValid(xmin) || TransactionIdIsValid(catalog_xmin))
		primary_has_standby_xmin = true;
	else
		primary_has_standby_xmin = false;
}

/*
 * Update shared memory status upon receiving a message from primary.
 *
 * 'walEnd' and 'sendTime' are the end-of-WAL and timestamp of the latest
 * message, reported by primary.
 */
static void // 更新一下共享内存中的信息。这两列信息都是来自主库的
ProcessWalSndrMessage(XLogRecPtr walEnd, TimestampTz sendTime)
{
	WalRcvData *walrcv = WalRcv;
	TimestampTz lastMsgReceiptTime = GetCurrentTimestamp();

	/* Update shared-memory status */
	SpinLockAcquire(&walrcv->mutex);
	if (walrcv->latestWalEnd < walEnd)
		walrcv->latestWalEndTime = sendTime;
	walrcv->latestWalEnd = walEnd;
	walrcv->lastMsgSendTime = sendTime;
	walrcv->lastMsgReceiptTime = lastMsgReceiptTime;
	SpinLockRelease(&walrcv->mutex);

	if (message_level_is_interesting(DEBUG2))
	{
		char	   *sendtime;
		char	   *receipttime;
		int			applyDelay;

		/* Copy because timestamptz_to_str returns a static buffer */
		sendtime = pstrdup(timestamptz_to_str(sendTime));
		receipttime = pstrdup(timestamptz_to_str(lastMsgReceiptTime));
		applyDelay = GetReplicationApplyDelay();

		/* apply delay is not available */
		if (applyDelay == -1)
			elog(DEBUG2, "sendtime %s receipttime %s replication apply delay (N/A) transfer latency %d ms",
				 sendtime,
				 receipttime,
				 GetReplicationTransferLatency());
		else
			elog(DEBUG2, "sendtime %s receipttime %s replication apply delay %d ms transfer latency %d ms",
				 sendtime,
				 receipttime,
				 applyDelay,
				 GetReplicationTransferLatency());

		pfree(sendtime);
		pfree(receipttime);
	}
}

/*
 * Compute the next wakeup time for a given wakeup reason.  Can be called to
 * initialize a wakeup time, to adjust it for the next wakeup, or to
 * reinitialize it when GUCs have changed.  We ask the caller to pass in the
 * value of "now" because this frequently avoids multiple calls of
 * GetCurrentTimestamp().  It had better be a reasonably up-to-date value
 * though.
 */
static void // 为每一种原因都指定一个超时时间，这需要wal_receiver_timeout和wal_receiver_status_interval超时时间设置好
WalRcvComputeNextWakeup(WalRcvWakeupReason reason, TimestampTz now) 
{
	switch (reason)
	{
		case WALRCV_WAKEUP_TERMINATE:
			if (wal_receiver_timeout <= 0)
				wakeup[reason] = TIMESTAMP_INFINITY; // TIMESTAMP_INFINITY是一个64位的上限，表示无限大
			else
				wakeup[reason] = TimestampTzPlusMilliseconds(now, wal_receiver_timeout); // 在指定的时间now上加上一个超时
			break;
		case WALRCV_WAKEUP_PING:
			if (wal_receiver_timeout <= 0)
				wakeup[reason] = TIMESTAMP_INFINITY;
			else
				wakeup[reason] = TimestampTzPlusMilliseconds(now, wal_receiver_timeout / 2);
			break;
		case WALRCV_WAKEUP_HSFEEDBACK:
			if (!hot_standby_feedback || wal_receiver_status_interval <= 0)
				wakeup[reason] = TIMESTAMP_INFINITY;
			else
				wakeup[reason] = TimestampTzPlusSeconds(now, wal_receiver_status_interval);
			break;
		case WALRCV_WAKEUP_REPLY:
			if (wal_receiver_status_interval <= 0)
				wakeup[reason] = TIMESTAMP_INFINITY;
			else
				wakeup[reason] = TimestampTzPlusSeconds(now, wal_receiver_status_interval);
			break;
			/* there's intentionally no default: here */
	}
}

/*
 * Wake up the walreceiver main loop.
 *
 * This is called by the startup process whenever interesting xlog records
 * are applied, so that walreceiver can check if it needs to send an apply
 * notification back to the primary which may be waiting in a COMMIT with
 * synchronous_commit = remote_apply.
 */
void
WalRcvForceReply(void)
{
	Latch	   *latch;

	WalRcv->force_reply = true;
	/* fetching the latch pointer might not be atomic, so use spinlock */
	SpinLockAcquire(&WalRcv->mutex);
	latch = WalRcv->latch;
	SpinLockRelease(&WalRcv->mutex);
	if (latch)
		SetLatch(latch);
}

/*
 * Return a string constant representing the state. This is used
 * in system functions and views, and should *not* be translated.
 */
static const char *
WalRcvGetStateString(WalRcvState state)
{
	switch (state)
	{
		case WALRCV_STOPPED:
			return "stopped";
		case WALRCV_STARTING:
			return "starting";
		case WALRCV_STREAMING:
			return "streaming";
		case WALRCV_WAITING:
			return "waiting";
		case WALRCV_RESTARTING:
			return "restarting";
		case WALRCV_STOPPING:
			return "stopping";
	}
	return "UNKNOWN";
}

/*
 * Returns activity of WAL receiver, including pid, state and xlog locations
 * received from the WAL sender of another server.
 */
Datum
pg_stat_get_wal_receiver(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum	   *values;
	bool	   *nulls;
	int			pid;
	bool		ready_to_display;
	WalRcvState state;
	XLogRecPtr	receive_start_lsn;
	TimeLineID	receive_start_tli;
	XLogRecPtr	written_lsn;
	XLogRecPtr	flushed_lsn;
	TimeLineID	received_tli;
	TimestampTz last_send_time;
	TimestampTz last_receipt_time;
	XLogRecPtr	latest_end_lsn;
	TimestampTz latest_end_time;
	char		sender_host[NI_MAXHOST];
	int			sender_port = 0;
	char		slotname[NAMEDATALEN];
	char		conninfo[MAXCONNINFO];

	/* Take a lock to ensure value consistency */
	SpinLockAcquire(&WalRcv->mutex);
	pid = (int) WalRcv->pid;
	ready_to_display = WalRcv->ready_to_display;
	state = WalRcv->walRcvState;
	receive_start_lsn = WalRcv->receiveStart;
	receive_start_tli = WalRcv->receiveStartTLI;
	flushed_lsn = WalRcv->flushedUpto;
	received_tli = WalRcv->receivedTLI;
	last_send_time = WalRcv->lastMsgSendTime;
	last_receipt_time = WalRcv->lastMsgReceiptTime;
	latest_end_lsn = WalRcv->latestWalEnd;
	latest_end_time = WalRcv->latestWalEndTime;
	strlcpy(slotname, (char *) WalRcv->slotname, sizeof(slotname));
	strlcpy(sender_host, (char *) WalRcv->sender_host, sizeof(sender_host));
	sender_port = WalRcv->sender_port;
	strlcpy(conninfo, (char *) WalRcv->conninfo, sizeof(conninfo));
	SpinLockRelease(&WalRcv->mutex);

	/*
	 * No WAL receiver (or not ready yet), just return a tuple with NULL
	 * values
	 */
	if (pid == 0 || !ready_to_display)
		PG_RETURN_NULL();

	/*
	 * Read "writtenUpto" without holding a spinlock.  Note that it may not be
	 * consistent with the other shared variables of the WAL receiver
	 * protected by a spinlock, but this should not be used for data integrity
	 * checks.
	 */
	written_lsn = pg_atomic_read_u64(&WalRcv->writtenUpto);

	/* determine result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	values = palloc0(sizeof(Datum) * tupdesc->natts);
	nulls = palloc0(sizeof(bool) * tupdesc->natts);

	/* Fetch values */
	values[0] = Int32GetDatum(pid);

	if (!has_privs_of_role(GetUserId(), ROLE_PG_READ_ALL_STATS))
	{
		/*
		 * Only superusers and roles with privileges of pg_read_all_stats can
		 * see details. Other users only get the pid value to know whether it
		 * is a WAL receiver, but no details.
		 */
		memset(&nulls[1], true, sizeof(bool) * (tupdesc->natts - 1));
	}
	else
	{
		values[1] = CStringGetTextDatum(WalRcvGetStateString(state));

		if (XLogRecPtrIsInvalid(receive_start_lsn))
			nulls[2] = true;
		else
			values[2] = LSNGetDatum(receive_start_lsn);
		values[3] = Int32GetDatum(receive_start_tli);
		if (XLogRecPtrIsInvalid(written_lsn))
			nulls[4] = true;
		else
			values[4] = LSNGetDatum(written_lsn);
		if (XLogRecPtrIsInvalid(flushed_lsn))
			nulls[5] = true;
		else
			values[5] = LSNGetDatum(flushed_lsn);
		values[6] = Int32GetDatum(received_tli);
		if (last_send_time == 0)
			nulls[7] = true;
		else
			values[7] = TimestampTzGetDatum(last_send_time);
		if (last_receipt_time == 0)
			nulls[8] = true;
		else
			values[8] = TimestampTzGetDatum(last_receipt_time);
		if (XLogRecPtrIsInvalid(latest_end_lsn))
			nulls[9] = true;
		else
			values[9] = LSNGetDatum(latest_end_lsn);
		if (latest_end_time == 0)
			nulls[10] = true;
		else
			values[10] = TimestampTzGetDatum(latest_end_time);
		if (*slotname == '\0')
			nulls[11] = true;
		else
			values[11] = CStringGetTextDatum(slotname);
		if (*sender_host == '\0')
			nulls[12] = true;
		else
			values[12] = CStringGetTextDatum(sender_host);
		if (sender_port == 0)
			nulls[13] = true;
		else
			values[13] = Int32GetDatum(sender_port);
		if (*conninfo == '\0')
			nulls[14] = true;
		else
			values[14] = CStringGetTextDatum(conninfo);
	}

	/* Returns the record as Datum */
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}
