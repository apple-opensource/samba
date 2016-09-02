/* 
   Unix SMB/Netbios implementation.
   Version 2.2.x
   oplock processing
   Copyright (C) Andrew Tridgell 1992-1998
   Copyright (C) Jeremy Allison 1998 - 2001
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "includes.h"

/* Oplock ipc UDP socket. */
static int oplock_sock = -1;
uint16 global_oplock_port = 0;

/* Current number of oplocks we have outstanding. */
static int32 exclusive_oplocks_open = 0;
static int32 level_II_oplocks_open = 0;
BOOL global_client_failed_oplock_break = False;
BOOL global_oplock_break = False;

extern int smb_read_error;

static struct kernel_oplocks *koplocks;

static BOOL oplock_break(SMB_DEV_T dev, SMB_INO_T inode, unsigned long file_id, BOOL local);

/****************************************************************************
 Get the number of current exclusive oplocks.
****************************************************************************/

int32 get_number_of_exclusive_open_oplocks(void)
{
  return exclusive_oplocks_open;
}

/****************************************************************************
 Return True if an oplock message is pending.
****************************************************************************/

BOOL oplock_message_waiting(fd_set *fds)
{
	if (koplocks && koplocks->msg_waiting(fds))
		return True;

	if (FD_ISSET(oplock_sock, fds))
		return True;

	return False;
}

/****************************************************************************
 Read an oplock break message from either the oplock UDP fd or the
 kernel (if kernel oplocks are supported).

 If timeout is zero then *fds contains the file descriptors that
 are ready to be read and acted upon. If timeout is non-zero then
 *fds contains the file descriptors to be selected on for read.
 The timeout is in milliseconds

****************************************************************************/

BOOL receive_local_message(fd_set *fds, char *buffer, int buffer_len, int timeout)
{
	struct sockaddr_in from;
	int fromlen = sizeof(from);
	int32 msg_len = 0;

	smb_read_error = 0;

	if(timeout != 0) {
		struct timeval to;
		int selrtn;
		int maxfd = oplock_sock;

		if (koplocks && koplocks->notification_fd != -1) {
			FD_SET(koplocks->notification_fd, fds);
			maxfd = MAX(maxfd, koplocks->notification_fd);
		}

		to.tv_sec = timeout / 1000;
		to.tv_usec = (timeout % 1000) * 1000;

		selrtn = sys_select(maxfd+1,fds,NULL,NULL,&to);

		if (selrtn == -1 && errno == EINTR) {
			/* could be a kernel oplock interrupt */
			if (koplocks && koplocks->msg_waiting(fds)) {
				return koplocks->receive_message(fds, buffer, buffer_len);
			}
		}

		/* Check if error */
		if(selrtn == -1) {
			/* something is wrong. Maybe the socket is dead? */
			smb_read_error = READ_ERROR;
			return False;
		}

		/* Did we timeout ? */
		if (selrtn == 0) {
			smb_read_error = READ_TIMEOUT;
			return False;
		}
	}

	if (koplocks && koplocks->msg_waiting(fds)) {
		return koplocks->receive_message(fds, buffer, buffer_len);
	}

	if (!FD_ISSET(oplock_sock, fds))
		return False;

	/*
	 * From here down we deal with the smbd <--> smbd
	 * oplock break protocol only.
	 */

	/*
	 * Read a loopback udp message.
	 */
	msg_len = recvfrom(oplock_sock, &buffer[OPBRK_CMD_HEADER_LEN],
						buffer_len - OPBRK_CMD_HEADER_LEN, 0, (struct sockaddr *)&from, &fromlen);

	if(msg_len < 0) {
		DEBUG(0,("receive_local_message. Error in recvfrom. (%s).\n",strerror(errno)));
		return False;
	}

	/* Validate message length. */
	if(msg_len > (buffer_len - OPBRK_CMD_HEADER_LEN)) {
		DEBUG(0,("receive_local_message: invalid msg_len (%d) max can be %d\n", msg_len,
			buffer_len  - OPBRK_CMD_HEADER_LEN));
		return False;
	}

	/* Validate message from address (must be localhost). */
	if(from.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
		DEBUG(0,("receive_local_message: invalid 'from' address \
(was %lx should be 127.0.0.1)\n", (long)from.sin_addr.s_addr));
		return False;
	}

	/* Setup the message header */
	SIVAL(buffer,OPBRK_CMD_LEN_OFFSET,msg_len);
	SSVAL(buffer,OPBRK_CMD_PORT_OFFSET,ntohs(from.sin_port));

	return True;
}

/****************************************************************************
 Attempt to set an oplock on a file. Always succeeds if kernel oplocks are
 disabled (just sets flags). Returns True if oplock set.
****************************************************************************/

BOOL set_file_oplock(files_struct *fsp, int oplock_type)
{
	if (koplocks && !koplocks->set_oplock(fsp, oplock_type))
		return False;

	fsp->oplock_type = oplock_type;
	fsp->sent_oplock_break = NO_BREAK_SENT;
	if (oplock_type == LEVEL_II_OPLOCK)
		level_II_oplocks_open++;
	else
		exclusive_oplocks_open++;

	DEBUG(5,("set_file_oplock: granted oplock on file %s, dev = %x, inode = %.0f, file_id = %lu, \
tv_sec = %x, tv_usec = %x\n",
		 fsp->fsp_name, (unsigned int)fsp->dev, (double)fsp->inode, fsp->file_id,
		 (int)fsp->open_time.tv_sec, (int)fsp->open_time.tv_usec ));

	return True;
}

/****************************************************************************
 Attempt to release an oplock on a file. Decrements oplock count.
****************************************************************************/

void release_file_oplock(files_struct *fsp)
{
	if (koplocks)
		koplocks->release_oplock(fsp);

	if (fsp->oplock_type == LEVEL_II_OPLOCK)
		level_II_oplocks_open--;
	else
		exclusive_oplocks_open--;
	
	fsp->oplock_type = NO_OPLOCK;
	fsp->sent_oplock_break = NO_BREAK_SENT;
	
	flush_write_cache(fsp, OPLOCK_RELEASE_FLUSH);
}

/****************************************************************************
 Attempt to downgrade an oplock on a file. Doesn't decrement oplock count.
****************************************************************************/

static void downgrade_file_oplock(files_struct *fsp)
{
	if (koplocks)
		koplocks->release_oplock(fsp);
	fsp->oplock_type = LEVEL_II_OPLOCK;
	exclusive_oplocks_open--;
	level_II_oplocks_open++;
	fsp->sent_oplock_break = NO_BREAK_SENT;
}

/****************************************************************************
 Remove a file oplock. Copes with level II and exclusive.
 Locks then unlocks the share mode lock. Client can decide to go directly
 to none even if a "break-to-level II" was sent.
****************************************************************************/

BOOL remove_oplock(files_struct *fsp, BOOL break_to_none)
{
	SMB_DEV_T dev = fsp->dev;
	SMB_INO_T inode = fsp->inode;
	BOOL ret = True;

	/* Remove the oplock flag from the sharemode. */
	if (lock_share_entry_fsp(fsp) == False) {
		DEBUG(0,("remove_oplock: failed to lock share entry for file %s\n",
			 fsp->fsp_name ));
		ret = False;
	}

	if (fsp->sent_oplock_break == EXCLUSIVE_BREAK_SENT || break_to_none) {
		/*
		 * Deal with a reply when a break-to-none was sent.
		 */

		if(remove_share_oplock(fsp)==False) {
			DEBUG(0,("remove_oplock: failed to remove share oplock for file %s fnum %d, \
dev = %x, inode = %.0f\n", fsp->fsp_name, fsp->fnum, (unsigned int)dev, (double)inode));
			ret = False;
		}

		release_file_oplock(fsp);
	} else {
		/*
		 * Deal with a reply when a break-to-level II was sent.
		 */
		if(downgrade_share_oplock(fsp)==False) {
			DEBUG(0,("remove_oplock: failed to downgrade share oplock for file %s fnum %d, \
dev = %x, inode = %.0f\n", fsp->fsp_name, fsp->fnum, (unsigned int)dev, (double)inode));
			ret = False;
		}
		
		downgrade_file_oplock(fsp);
	}

	unlock_share_entry_fsp(fsp);
	return ret;
}

/****************************************************************************
 Setup the listening set of file descriptors for an oplock break
 message either from the UDP socket or from the kernel. Returns the maximum
 fd used.
****************************************************************************/

int setup_oplock_select_set( fd_set *fds)
{
	int maxfd = oplock_sock;

	if(oplock_sock == -1)
		return 0;

	FD_SET(oplock_sock,fds);

	if (koplocks && koplocks->notification_fd != -1) {
		FD_SET(koplocks->notification_fd, fds);
		maxfd = MAX(maxfd, koplocks->notification_fd);
	}

	return maxfd;
}

/****************************************************************************
 Process an oplock break message - whether it came from the UDP socket
 or from the kernel.
****************************************************************************/

BOOL process_local_message(char *buffer, int buf_size)
{
	int32 msg_len;
	uint16 from_port;
	char *msg_start;
	pid_t remotepid;
	SMB_DEV_T dev;
	SMB_INO_T inode;
	unsigned long file_id;
	uint16 break_cmd_type;

	msg_len = IVAL(buffer,OPBRK_CMD_LEN_OFFSET);
	from_port = SVAL(buffer,OPBRK_CMD_PORT_OFFSET);

	msg_start = &buffer[OPBRK_CMD_HEADER_LEN];

	DEBUG(5,("process_local_message: Got a message of length %d from port (%d)\n", 
		msg_len, from_port));

	/* 
	 * Pull the info out of the requesting packet.
	 */

	break_cmd_type = SVAL(msg_start,OPBRK_MESSAGE_CMD_OFFSET);

	switch(break_cmd_type) {
		case KERNEL_OPLOCK_BREAK_CMD:
			if (!koplocks) {
				DEBUG(0,("unexpected kernel oplock break!\n"));
				break;
			} 
			if (!koplocks->parse_message(msg_start, msg_len, &inode, &dev, &file_id)) {
				DEBUG(0,("kernel oplock break parse failure!\n"));
			}
			break;

		case OPLOCK_BREAK_CMD:
		case LEVEL_II_OPLOCK_BREAK_CMD:

			/* Ensure that the msg length is correct. */
			if(msg_len != OPLOCK_BREAK_MSG_LEN) {
				DEBUG(0,("process_local_message: incorrect length for OPLOCK_BREAK_CMD (was %d, should be %d).\n",
					(int)msg_len, (int)OPLOCK_BREAK_MSG_LEN));
				return False;
			}

			memcpy((char *)&remotepid, msg_start+OPLOCK_BREAK_PID_OFFSET,sizeof(remotepid));
			memcpy((char *)&inode, msg_start+OPLOCK_BREAK_INODE_OFFSET,sizeof(inode));
			memcpy((char *)&dev, msg_start+OPLOCK_BREAK_DEV_OFFSET,sizeof(dev));
			memcpy((char *)&file_id, msg_start+OPLOCK_BREAK_FILEID_OFFSET,sizeof(file_id));

			DEBUG(5,("process_local_message: (%s) oplock break request from \
pid %d, port %d, dev = %x, inode = %.0f, file_id = %lu\n",
				(break_cmd_type == OPLOCK_BREAK_CMD) ? "exclusive" : "level II",
				(int)remotepid, from_port, (unsigned int)dev, (double)inode, file_id));
			break;

		/* 
		 * Keep this as a debug case - eventually we can remove it.
		 */
		case 0x8001:
			DEBUG(0,("process_local_message: Received unsolicited break \
reply - dumping info.\n"));

			if(msg_len != OPLOCK_BREAK_MSG_LEN) {
				DEBUG(0,("process_local_message: ubr: incorrect length for reply \
(was %d, should be %d).\n", (int)msg_len, (int)OPLOCK_BREAK_MSG_LEN));
				return False;
			}

			memcpy((char *)&inode, msg_start+OPLOCK_BREAK_INODE_OFFSET,sizeof(inode));
			memcpy((char *)&remotepid, msg_start+OPLOCK_BREAK_PID_OFFSET,sizeof(remotepid));
			memcpy((char *)&dev, msg_start+OPLOCK_BREAK_DEV_OFFSET,sizeof(dev));
			memcpy((char *)&file_id, msg_start+OPLOCK_BREAK_FILEID_OFFSET,sizeof(file_id));

			DEBUG(0,("process_local_message: unsolicited oplock break reply from \
pid %d, port %d, dev = %x, inode = %.0f, file_id = %lu\n",
				(int)remotepid, from_port, (unsigned int)dev, (double)inode, file_id));

			return False;

		default:
			DEBUG(0,("process_local_message: unknown UDP message command code (%x) - ignoring.\n",
				(unsigned int)SVAL(msg_start,0)));
			return False;
	}

	/*
	 * Now actually process the break request.
	 */

	if((exclusive_oplocks_open + level_II_oplocks_open) != 0) {
		if (oplock_break(dev, inode, file_id, False) == False) {
			DEBUG(0,("process_local_message: oplock break failed.\n"));
			return False;
		}
	} else {
		/*
		 * If we have no record of any currently open oplocks,
		 * it's not an error, as a close command may have
		 * just been issued on the file that was oplocked.
		 * Just log a message and return success in this case.
		 */
		DEBUG(3,("process_local_message: oplock break requested with no outstanding \
oplocks. Returning success.\n"));
	}

	/* 
	 * Do the appropriate reply - none in the kernel or level II case.
	 */

	if(SVAL(msg_start,OPBRK_MESSAGE_CMD_OFFSET) == OPLOCK_BREAK_CMD) {
		struct sockaddr_in toaddr;

		/* Send the message back after OR'ing in the 'REPLY' bit. */
		SSVAL(msg_start,OPBRK_MESSAGE_CMD_OFFSET,OPLOCK_BREAK_CMD | CMD_REPLY);

		memset((char *)&toaddr,'\0',sizeof(toaddr));
		toaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		toaddr.sin_port = htons(from_port);
		toaddr.sin_family = AF_INET;

		if(sendto( oplock_sock, msg_start, OPLOCK_BREAK_MSG_LEN, 0,
				(struct sockaddr *)&toaddr, sizeof(toaddr)) < 0) {
			DEBUG(0,("process_local_message: sendto process %d failed. Errno was %s\n",
				(int)remotepid, strerror(errno)));
			return False;
		}

		DEBUG(5,("process_local_message: oplock break reply sent to \
pid %d, port %d, for file dev = %x, inode = %.0f, file_id = %lu\n",
			(int)remotepid, from_port, (unsigned int)dev, (double)inode, file_id));
	}

	return True;
}

/****************************************************************************
 Set up an oplock break message.
****************************************************************************/

static void prepare_break_message(char *outbuf, files_struct *fsp, BOOL level2)
{
	memset(outbuf,'\0',smb_size);
	set_message(outbuf,8,0,True);

	SCVAL(outbuf,smb_com,SMBlockingX);
	SSVAL(outbuf,smb_tid,fsp->conn->cnum);
	SSVAL(outbuf,smb_pid,0xFFFF);
	SSVAL(outbuf,smb_uid,0);
	SSVAL(outbuf,smb_mid,0xFFFF);
	SCVAL(outbuf,smb_vwv0,0xFF);
	SSVAL(outbuf,smb_vwv2,fsp->fnum);
	SCVAL(outbuf,smb_vwv3,LOCKING_ANDX_OPLOCK_RELEASE);
	SCVAL(outbuf,smb_vwv3+1,level2 ? OPLOCKLEVEL_II : OPLOCKLEVEL_NONE);
}

/****************************************************************************
 Function to do the waiting before sending a local break.
****************************************************************************/

static void wait_before_sending_break(BOOL local_request)
{
	extern struct timeval smb_last_time;

	if(local_request) {
		struct timeval cur_tv;
		long wait_left = (long)lp_oplock_break_wait_time();

		if (wait_left == 0)
			return;

		GetTimeOfDay(&cur_tv);

		wait_left -= ((cur_tv.tv_sec - smb_last_time.tv_sec)*1000) +
                ((cur_tv.tv_usec - smb_last_time.tv_usec)/1000);

		if(wait_left > 0) {
			wait_left = MIN(wait_left, 1000);
			sys_usleep(wait_left * 1000);
		}
	}
}

/****************************************************************************
 Ensure that we have a valid oplock.
****************************************************************************/

static files_struct *initial_break_processing(SMB_DEV_T dev, SMB_INO_T inode, unsigned long file_id)
{
	files_struct *fsp = NULL;

	if( DEBUGLVL( 3 ) ) {
		dbgtext( "initial_break_processing: called for dev = %x, inode = %.0f file_id = %lu\n",
			(unsigned int)dev, (double)inode, file_id);
		dbgtext( "Current oplocks_open (exclusive = %d, levelII = %d)\n",
			exclusive_oplocks_open, level_II_oplocks_open );
	}

	/*
	 * We need to search the file open table for the
	 * entry containing this dev and inode, and ensure
	 * we have an oplock on it.
	 */

	fsp = file_find_dif(dev, inode, file_id);

	if(fsp == NULL) {
		/* The file could have been closed in the meantime - return success. */
		if( DEBUGLVL( 3 ) ) {
			dbgtext( "initial_break_processing: cannot find open file with " );
			dbgtext( "dev = %x, inode = %.0f file_id = %lu", (unsigned int)dev,
				(double)inode, file_id);
			dbgtext( "allowing break to succeed.\n" );
		}
		return NULL;
	}

	/* Ensure we have an oplock on the file */

	/*
	 * There is a potential race condition in that an oplock could
	 * have been broken due to another udp request, and yet there are
	 * still oplock break messages being sent in the udp message
	 * queue for this file. So return true if we don't have an oplock,
	 * as we may have just freed it.
	 */

	if(fsp->oplock_type == NO_OPLOCK) {
		if( DEBUGLVL( 3 ) ) {
			dbgtext( "initial_break_processing: file %s ", fsp->fsp_name );
			dbgtext( "(dev = %x, inode = %.0f, file_id = %lu) has no oplock.\n",
				(unsigned int)dev, (double)inode, fsp->file_id );
			dbgtext( "Allowing break to succeed regardless.\n" );
		}
		return NULL;
	}

	return fsp;
}

/****************************************************************************
 Process a level II oplock break directly.
****************************************************************************/

BOOL oplock_break_level2(files_struct *fsp, BOOL local_request, int token)
{
	extern uint32 global_client_caps;
	char outbuf[128];
	BOOL got_lock = False;
	SMB_DEV_T dev = fsp->dev;
	SMB_INO_T inode = fsp->inode;

	/*
	 * We can have a level II oplock even if the client is not
	 * level II oplock aware. In this case just remove the
	 * flags and don't send the break-to-none message to
	 * the client.
	 */

	if (global_client_caps & CAP_LEVEL_II_OPLOCKS) {
		/*
		 * If we are sending an oplock break due to an SMB sent
		 * by our own client we ensure that we wait at leat
		 * lp_oplock_break_wait_time() milliseconds before sending
		 * the packet. Sending the packet sooner can break Win9x
		 * and has reported to cause problems on NT. JRA.
		 */

		wait_before_sending_break(local_request);

		/* Prepare the SMBlockingX message. */

		prepare_break_message( outbuf, fsp, False);
		if (!send_smb(smbd_server_fd(), outbuf))
			exit_server("oplock_break_level2: send_smb failed.\n");
	}

	/*
	 * Now we must update the shared memory structure to tell
	 * everyone else we no longer have a level II oplock on 
	 * this open file. If local_request is true then token is
	 * the existing lock on the shared memory area.
	 */

	if(!local_request && lock_share_entry_fsp(fsp) == False) {
		DEBUG(0,("oplock_break_level2: unable to lock share entry for file %s\n", fsp->fsp_name ));
	} else {
		got_lock = True;
	}

	if(remove_share_oplock(fsp)==False) {
		DEBUG(0,("oplock_break_level2: unable to remove level II oplock for file %s\n", fsp->fsp_name ));
	}

	if (!local_request && got_lock)
		unlock_share_entry_fsp(fsp);

	fsp->oplock_type = NO_OPLOCK;
	level_II_oplocks_open--;

	if(level_II_oplocks_open < 0) {
		DEBUG(0,("oplock_break_level2: level_II_oplocks_open < 0 (%d). PANIC ERROR\n",
			level_II_oplocks_open));
		abort();
	}

	if( DEBUGLVL( 3 ) ) {
		dbgtext( "oplock_break_level2: returning success for " );
		dbgtext( "dev = %x, inode = %.0f, file_id = %lu\n", (unsigned int)dev, (double)inode, fsp->file_id );
		dbgtext( "Current level II oplocks_open = %d\n", level_II_oplocks_open );
	}

	return True;
}

/****************************************************************************
 Process an oplock break directly.
****************************************************************************/

static BOOL oplock_break(SMB_DEV_T dev, SMB_INO_T inode, unsigned long file_id, BOOL local_request)
{
	extern uint32 global_client_caps;
	extern struct current_user current_user;
	char *inbuf = NULL;
	char *outbuf = NULL;
	files_struct *fsp = NULL;
	time_t start_time;
	BOOL shutdown_server = False;
	BOOL oplock_timeout = False;
	connection_struct *saved_user_conn;
	connection_struct *saved_fsp_conn;
	int saved_vuid;
	pstring saved_dir; 
	int timeout = (OPLOCK_BREAK_TIMEOUT * 1000);
	pstring file_name;
	BOOL using_levelII;

	if((fsp = initial_break_processing(dev, inode, file_id)) == NULL)
		return True;

	/*
	 * Deal with a level II oplock going break to none separately.
	 */

	if (LEVEL_II_OPLOCK_TYPE(fsp->oplock_type))
		return oplock_break_level2(fsp, local_request, -1);

	/* Mark the oplock break as sent - we don't want to send twice! */
	if (fsp->sent_oplock_break) {
		if( DEBUGLVL( 0 ) ) {
			dbgtext( "oplock_break: ERROR: oplock_break already sent for " );
			dbgtext( "file %s ", fsp->fsp_name);
			dbgtext( "(dev = %x, inode = %.0f, file_id = %lu)\n", (unsigned int)dev, (double)inode, fsp->file_id );
		}

		/*
		 * We have to fail the open here as we cannot send another oplock break on
		 * this file whilst we are awaiting a response from the client - neither
		 * can we allow another open to succeed while we are waiting for the client.
		 */
		return False;
	}

	if(global_oplock_break) {
		DEBUG(0,("ABORT : ABORT : recursion in oplock_break !!!!!\n"));
		abort();
	}

	/*
	 * Now comes the horrid part. We must send an oplock break to the client,
	 * and then process incoming messages until we get a close or oplock release.
	 * At this point we know we need a new inbuf/outbuf buffer pair.
	 * We cannot use these staticaly as we may recurse into here due to
	 * messages crossing on the wire.
	 */

	if((inbuf = (char *)malloc(BUFFER_SIZE + LARGE_WRITEX_HDR_SIZE + SAFETY_MARGIN))==NULL) {
		DEBUG(0,("oplock_break: malloc fail for input buffer.\n"));
		return False;
	}

	if((outbuf = (char *)malloc(BUFFER_SIZE + LARGE_WRITEX_HDR_SIZE + SAFETY_MARGIN))==NULL) {
		DEBUG(0,("oplock_break: malloc fail for output buffer.\n"));
		SAFE_FREE(inbuf);
		return False;
	}

	/*
	 * If we are sending an oplock break due to an SMB sent
	 * by our own client we ensure that we wait at leat
	 * lp_oplock_break_wait_time() milliseconds before sending
	 * the packet. Sending the packet sooner can break Win9x
	 * and has reported to cause problems on NT. JRA.
	 */

	wait_before_sending_break(local_request);

	/* Prepare the SMBlockingX message. */

	if ((global_client_caps & CAP_LEVEL_II_OPLOCKS) && 
			!koplocks && /* NOTE: we force levelII off for kernel oplocks - this will change when it is supported */
			lp_level2_oplocks(SNUM(fsp->conn))) {
		using_levelII = True;
	} else {
		using_levelII = False;
	}

	prepare_break_message( outbuf, fsp, using_levelII);
	/* Remember if we just sent a break to level II on this file. */
	fsp->sent_oplock_break = using_levelII? LEVEL_II_BREAK_SENT:EXCLUSIVE_BREAK_SENT;

	if (!send_smb(smbd_server_fd(), outbuf))
		exit_server("oplock_break: send_smb failed.\n");

	/* We need this in case a readraw crosses on the wire. */
	global_oplock_break = True;
 
	/* Process incoming messages. */

	/*
	 * JRA - If we don't get a break from the client in OPLOCK_BREAK_TIMEOUT
	 * seconds we should just die....
	 */

	start_time = time(NULL);

	/*
	 * Save the information we need to re-become the
	 * user, then unbecome the user whilst we're doing this.
	 */
	saved_user_conn = current_user.conn;
	saved_vuid = current_user.vuid;
	saved_fsp_conn = fsp->conn;
	vfs_GetWd(saved_fsp_conn,saved_dir);
	change_to_root_user();
	/* Save the chain fnum. */
	file_chain_save();

	/*
	 * From Charles Hoch <hoch@exemplary.com>. If the break processing
	 * code closes the file (as it often does), then the fsp pointer here
	 * points to free()'d memory. We *must* revalidate fsp each time
	 * around the loop.
	 */

	pstrcpy(file_name, fsp->fsp_name);

	while((fsp = initial_break_processing(dev, inode, file_id)) &&
			OPEN_FSP(fsp) && EXCLUSIVE_OPLOCK_TYPE(fsp->oplock_type)) {
		if(receive_smb(smbd_server_fd(),inbuf, timeout) == False) {
			/*
			 * Die if we got an error.
			 */

			if (smb_read_error == READ_EOF) {
				DEBUG( 0, ( "oplock_break: end of file from client\n" ) );
				shutdown_server = True;
			} else if (smb_read_error == READ_ERROR) {
				DEBUG( 0, ("oplock_break: receive_smb error (%s)\n", strerror(errno)) );
				shutdown_server = True;
			} else if (smb_read_error == READ_TIMEOUT) {
				DEBUG( 0, ( "oplock_break: receive_smb timed out after %d seconds.\n", OPLOCK_BREAK_TIMEOUT ) );
				oplock_timeout = True;
			}

			DEBUGADD( 0, ( "oplock_break failed for file %s ", file_name ) );
			DEBUGADD( 0, ( "(dev = %x, inode = %.0f, file_id = %lu).\n",
				(unsigned int)dev, (double)inode, file_id));

			break;
		}

		/*
		 * There are certain SMB requests that we shouldn't allow
		 * to recurse. opens, renames and deletes are the obvious
		 * ones. This is handled in the switch_message() function.
		 * If global_oplock_break is set they will push the packet onto
		 * the pending smb queue and return -1 (no reply).
		 * JRA.
		 */

		process_smb(inbuf, outbuf);

		/*
		 * Die if we go over the time limit.
		 */

		if((time(NULL) - start_time) > OPLOCK_BREAK_TIMEOUT) {
			if( DEBUGLVL( 0 ) ) {
				dbgtext( "oplock_break: no break received from client " );
				dbgtext( "within %d seconds.\n", OPLOCK_BREAK_TIMEOUT );
				dbgtext( "oplock_break failed for file %s ", fsp->fsp_name );
				dbgtext( "(dev = %x, inode = %.0f, file_id = %lu).\n",	
					(unsigned int)dev, (double)inode, file_id );
			}
			oplock_timeout = True;
			break;
		}
	}

	/*
	 * Go back to being the user who requested the oplock
	 * break.
	 */
	if((saved_user_conn != NULL) && (saved_vuid != UID_FIELD_INVALID) && !change_to_user(saved_user_conn, saved_vuid)) {
		DEBUG( 0, ( "oplock_break: unable to re-become user!" ) );
		DEBUGADD( 0, ( "Shutting down server\n" ) );
		close(oplock_sock);
		exit_server("unable to re-become user");
	}

	/* Including the directory. */
	vfs_ChDir(saved_fsp_conn,saved_dir);

	/* Restore the chain fnum. */
	file_chain_restore();

	/* Free the buffers we've been using to recurse. */
	SAFE_FREE(inbuf);
	SAFE_FREE(outbuf);

	/* We need this in case a readraw crossed on the wire. */
	if(global_oplock_break)
		global_oplock_break = False;

	/*
	 * If the client timed out then clear the oplock (or go to level II)
	 * and continue. This seems to be what NT does and is better than dropping
	 * the connection.
	 */

	if(oplock_timeout && (fsp = initial_break_processing(dev, inode, file_id)) &&
			OPEN_FSP(fsp) && EXCLUSIVE_OPLOCK_TYPE(fsp->oplock_type)) {
		DEBUG(0,("oplock_break: client failure in oplock break in file %s\n", fsp->fsp_name));
		remove_oplock(fsp,True);
		global_client_failed_oplock_break = True; /* Never grant this client an oplock again. */
	}

	/*
	 * If the client had an error we must die.
	 */

	if(shutdown_server) {
		DEBUG( 0, ( "oplock_break: client failure in break - " ) );
		DEBUGADD( 0, ( "shutting down this smbd.\n" ) );
		close(oplock_sock);
		exit_server("oplock break failure");
	}

	/* Santity check - remove this later. JRA */
	if(exclusive_oplocks_open < 0) {
		DEBUG(0,("oplock_break: exclusive_oplocks_open < 0 (%d). PANIC ERROR\n", exclusive_oplocks_open));
		abort();
	}

	if( DEBUGLVL( 3 ) ) {
		dbgtext( "oplock_break: returning success for " );
		dbgtext( "dev = %x, inode = %.0f, file_id = %lu\n", (unsigned int)dev, (double)inode, file_id );
		dbgtext( "Current exclusive_oplocks_open = %d\n", exclusive_oplocks_open );
	}

	return True;
}

/****************************************************************************
Send an oplock break message to another smbd process. If the oplock is held 
by the local smbd then call the oplock break function directly.
****************************************************************************/

BOOL request_oplock_break(share_mode_entry *share_entry)
{
	char op_break_msg[OPLOCK_BREAK_MSG_LEN];
	struct sockaddr_in addr_out;
	pid_t pid = sys_getpid();
	time_t start_time;
	int time_left;
	SMB_DEV_T dev = share_entry->dev;
	SMB_INO_T inode = share_entry->inode;
	unsigned long file_id = share_entry->share_file_id;

	if(pid == share_entry->pid) {
		/* We are breaking our own oplock, make sure it's us. */
		if(share_entry->op_port != global_oplock_port) {
			DEBUG(0,("request_oplock_break: corrupt share mode entry - pid = %d, port = %d \
should be %d\n", (int)pid, share_entry->op_port, global_oplock_port));
			return False;
		}

		DEBUG(5,("request_oplock_break: breaking our own oplock\n"));

#if 1 /* JRA PARANOIA TEST.... */
		{
			files_struct *fsp = file_find_dif(dev, inode, file_id);
			if (!fsp) {
				DEBUG(0,("request_oplock_break: PANIC : breaking our own oplock requested for \
dev = %x, inode = %.0f, file_id = %lu and no fsp found !\n",
            (unsigned int)dev, (double)inode, file_id ));
				smb_panic("request_oplock_break: no fsp found for our own oplock\n");
			}
		}
#endif /* END JRA PARANOIA TEST... */

		/* Call oplock break direct. */
		return oplock_break(dev, inode, file_id, True);
	}

	/* We need to send a OPLOCK_BREAK_CMD message to the port in the share mode entry. */

	if (LEVEL_II_OPLOCK_TYPE(share_entry->op_type)) {
		SSVAL(op_break_msg,OPBRK_MESSAGE_CMD_OFFSET,LEVEL_II_OPLOCK_BREAK_CMD);
	} else {
		SSVAL(op_break_msg,OPBRK_MESSAGE_CMD_OFFSET,OPLOCK_BREAK_CMD);
	}

	memcpy(op_break_msg+OPLOCK_BREAK_PID_OFFSET,(char *)&pid,sizeof(pid));
	memcpy(op_break_msg+OPLOCK_BREAK_DEV_OFFSET,(char *)&dev,sizeof(dev));
	memcpy(op_break_msg+OPLOCK_BREAK_INODE_OFFSET,(char *)&inode,sizeof(inode));
	memcpy(op_break_msg+OPLOCK_BREAK_FILEID_OFFSET,(char *)&file_id,sizeof(file_id));

	/* Set the address and port. */
	memset((char *)&addr_out,'\0',sizeof(addr_out));
	addr_out.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr_out.sin_port = htons( share_entry->op_port );
	addr_out.sin_family = AF_INET;
   
	if( DEBUGLVL( 3 ) ) {
		dbgtext( "request_oplock_break: sending a oplock break message to " );
		dbgtext( "pid %d on port %d ", (int)share_entry->pid, share_entry->op_port );
		dbgtext( "for dev = %x, inode = %.0f, file_id = %lu\n",
            (unsigned int)dev, (double)inode, file_id );
	}

	if(sendto(oplock_sock,op_break_msg,OPLOCK_BREAK_MSG_LEN,0,
			(struct sockaddr *)&addr_out,sizeof(addr_out)) < 0) {
		if( DEBUGLVL( 0 ) ) {
			dbgtext( "request_oplock_break: failed when sending a oplock " );
			dbgtext( "break message to pid %d ", (int)share_entry->pid );
			dbgtext( "on port %d ", share_entry->op_port );
			dbgtext( "for dev = %x, inode = %.0f, file_id = %lu\n",
          (unsigned int)dev, (double)inode, file_id );
			dbgtext( "Error was %s\n", strerror(errno) );
		}
		return False;
	}

	/*
	 * If we just sent a message to a level II oplock share entry then
	 * we are done and may return.
	 */

	if (LEVEL_II_OPLOCK_TYPE(share_entry->op_type)) {
		DEBUG(3,("request_oplock_break: sent break message to level II entry.\n"));
		return True;
	}

	/*
	 * Now we must await the oplock broken message coming back
	 * from the target smbd process. Timeout if it fails to
	 * return in (OPLOCK_BREAK_TIMEOUT + OPLOCK_BREAK_TIMEOUT_FUDGEFACTOR) seconds.
	 * While we get messages that aren't ours, loop.
	 */

	start_time = time(NULL);
	time_left = OPLOCK_BREAK_TIMEOUT+OPLOCK_BREAK_TIMEOUT_FUDGEFACTOR;

	while(time_left >= 0) {
		char op_break_reply[OPBRK_CMD_HEADER_LEN+OPLOCK_BREAK_MSG_LEN];
		uint16 reply_from_port;
		char *reply_msg_start;
		fd_set fds;

		FD_ZERO(&fds);
		FD_SET(oplock_sock,&fds);

		if (koplocks && koplocks->notification_fd != -1) {
			FD_SET(koplocks->notification_fd, &fds);
		}

		if(receive_local_message(&fds, op_break_reply, sizeof(op_break_reply),
				time_left ? time_left * 1000 : 1) == False) {
			if(smb_read_error == READ_TIMEOUT) {
				if( DEBUGLVL( 0 ) ) {
					dbgtext( "request_oplock_break: no response received to oplock " );
					dbgtext( "break request to pid %d ", (int)share_entry->pid );
					dbgtext( "on port %d ", share_entry->op_port );
					dbgtext( "for dev = %x, inode = %.0f, file_id = %lu\n",
							(unsigned int)dev, (double)inode, file_id );
				}

				/*
				 * This is a hack to make handling of failing clients more robust.
				 * If a oplock break response message is not received in the timeout
				 * period we may assume that the smbd servicing that client holding
				 * the oplock has died and the client changes were lost anyway, so
				 * we should continue to try and open the file.
				 */
				break;
			} else {
				if( DEBUGLVL( 0 ) ) {
					dbgtext( "request_oplock_break: error in response received " );
					dbgtext( "to oplock break request to pid %d ", (int)share_entry->pid );
					dbgtext( "on port %d ", share_entry->op_port );
					dbgtext( "for dev = %x, inode = %.0f, file_id = %lu\n",
						(unsigned int)dev, (double)inode, file_id );
					dbgtext( "Error was (%s).\n", strerror(errno) );
				}
			}
			return False;
		}

		reply_from_port = SVAL(op_break_reply,OPBRK_CMD_PORT_OFFSET);
		reply_msg_start = &op_break_reply[OPBRK_CMD_HEADER_LEN];

		/*
		 * Test to see if this is the reply we are awaiting.
		 */
		if((SVAL(reply_msg_start,OPBRK_MESSAGE_CMD_OFFSET) & CMD_REPLY) &&
			((SVAL(reply_msg_start,OPBRK_MESSAGE_CMD_OFFSET) & ~CMD_REPLY) == OPLOCK_BREAK_CMD) &&
			(reply_from_port == share_entry->op_port) && 
			(memcmp(&reply_msg_start[OPLOCK_BREAK_PID_OFFSET], &op_break_msg[OPLOCK_BREAK_PID_OFFSET],
				OPLOCK_BREAK_MSG_LEN - OPLOCK_BREAK_PID_OFFSET) == 0)) {

			/*
			 * This is the reply we've been waiting for.
			 */
			break;
		} else {
			/*
			 * This is another message - a break request.
			 * Note that both kernel oplock break requests
			 * and UDP inter-smbd oplock break requests will
			 * be processed here.
			 *
			 * Process it to prevent potential deadlock.
			 * Note that the code in switch_message() prevents
			 * us from recursing into here as any SMB requests
			 * we might process that would cause another oplock
			 * break request to be made will be queued.
			 * JRA.
			 */

			process_local_message(op_break_reply, sizeof(op_break_reply));
		}

		time_left -= (time(NULL) - start_time);
	}

	DEBUG(3,("request_oplock_break: broke oplock.\n"));

	return True;
}

/****************************************************************************
  Attempt to break an oplock on a file (if oplocked).
  Returns True if the file was closed as a result of
  the oplock break, False otherwise.
  Used as a last ditch attempt to free a space in the 
  file table when we have run out.
****************************************************************************/

BOOL attempt_close_oplocked_file(files_struct *fsp)
{
	DEBUG(5,("attempt_close_oplocked_file: checking file %s.\n", fsp->fsp_name));

	if (EXCLUSIVE_OPLOCK_TYPE(fsp->oplock_type) && !fsp->sent_oplock_break && (fsp->fd != -1)) {
		/* Try and break the oplock. */
		if (oplock_break(fsp->dev, fsp->inode, fsp->file_id, True)) {
			if(file_find_fsp(fsp) == NULL) /* Did the oplock break close the file ? */
				return True;
		}
	}

	return False;
}

/****************************************************************************
 This function is called on any file modification or lock request. If a file
 is level 2 oplocked then it must tell all other level 2 holders to break to none.
****************************************************************************/

void release_level_2_oplocks_on_change(files_struct *fsp)
{
	share_mode_entry *share_list = NULL;
	pid_t pid = sys_getpid();
	int token = -1;
	int num_share_modes = 0;
	int i;

	/*
	 * If this file is level II oplocked then we need
	 * to grab the shared memory lock and inform all
	 * other files with a level II lock that they need
	 * to flush their read caches. We keep the lock over
	 * the shared memory area whilst doing this.
	 */

	if (!LEVEL_II_OPLOCK_TYPE(fsp->oplock_type))
		return;

	if (lock_share_entry_fsp(fsp) == False) {
		DEBUG(0,("release_level_2_oplocks_on_change: failed to lock share mode entry for file %s.\n", fsp->fsp_name ));
	}

	num_share_modes = get_share_modes(fsp->conn, fsp->dev, fsp->inode, &share_list);

	DEBUG(10,("release_level_2_oplocks_on_change: num_share_modes = %d\n", 
			num_share_modes ));

	for(i = 0; i < num_share_modes; i++) {
		share_mode_entry *share_entry = &share_list[i];

		/*
		 * As there could have been multiple writes waiting at the lock_share_entry
		 * gate we may not be the first to enter. Hence the state of the op_types
		 * in the share mode entries may be partly NO_OPLOCK and partly LEVEL_II
		 * oplock. It will do no harm to re-send break messages to those smbd's
		 * that are still waiting their turn to remove their LEVEL_II state, and
		 * also no harm to ignore existing NO_OPLOCK states. JRA.
		 */

		DEBUG(10,("release_level_2_oplocks_on_change: share_entry[%i]->op_type == %d\n",
				i, share_entry->op_type ));

		if (share_entry->op_type == NO_OPLOCK)
			continue;

		/* Paranoia .... */
		if (EXCLUSIVE_OPLOCK_TYPE(share_entry->op_type)) {
			DEBUG(0,("release_level_2_oplocks_on_change: PANIC. share mode entry %d is an exlusive oplock !\n", i ));
			unlock_share_entry(fsp->conn, fsp->dev, fsp->inode);
			abort();
		}

		/*
		 * Check if this is a file we have open (including the
		 * file we've been called to do write_file on. If so
		 * then break it directly without releasing the lock.
		 */

		if (pid == share_entry->pid) {
			files_struct *new_fsp = file_find_dif(share_entry->dev, share_entry->inode, share_entry->share_file_id);

			/* Paranoia check... */
			if(new_fsp == NULL) {
				DEBUG(0,("release_level_2_oplocks_on_change: PANIC. share mode entry %d is not a local file !\n", i ));
				unlock_share_entry(fsp->conn, fsp->dev, fsp->inode);
				abort();
			}

			DEBUG(10,("release_level_2_oplocks_on_change: breaking our own oplock.\n"));

			oplock_break_level2(new_fsp, True, token);

		} else {

			/*
			 * This is a remote file and so we send an asynchronous
			 * message.
			 */

			DEBUG(10,("release_level_2_oplocks_on_change: breaking remote oplock.\n"));
			request_oplock_break(share_entry);
		}
	}

	SAFE_FREE(share_list);
	unlock_share_entry_fsp(fsp);

	/* Paranoia check... */
	if (LEVEL_II_OPLOCK_TYPE(fsp->oplock_type)) {
		DEBUG(0,("release_level_2_oplocks_on_change: PANIC. File %s still has a level II oplock.\n", fsp->fsp_name));
		smb_panic("release_level_2_oplocks_on_change");
	}
}

/****************************************************************************
setup oplocks for this process
****************************************************************************/

BOOL init_oplocks(void)
{
	struct sockaddr_in sock_name;
	socklen_t len = sizeof(sock_name);

	DEBUG(3,("open_oplock_ipc: opening loopback UDP socket.\n"));

	/* Open a lookback UDP socket on a random port. */
	oplock_sock = open_socket_in(SOCK_DGRAM, 0, 0, htonl(INADDR_LOOPBACK),False);
	if (oplock_sock == -1) {
		DEBUG(0,("open_oplock_ipc: Failed to get local UDP socket for \
address %lx. Error was %s\n", (long)htonl(INADDR_LOOPBACK), strerror(errno)));
		global_oplock_port = 0;
		return(False);
	}

	/* Find out the transient UDP port we have been allocated. */
	if(getsockname(oplock_sock, (struct sockaddr *)&sock_name, &len)<0) {
		DEBUG(0,("open_oplock_ipc: Failed to get local UDP port. Error was %s\n",
			 strerror(errno)));
		close(oplock_sock);
		oplock_sock = -1;
		global_oplock_port = 0;
		return False;
	}
	global_oplock_port = ntohs(sock_name.sin_port);

	if (lp_kernel_oplocks()) {
#if HAVE_KERNEL_OPLOCKS_IRIX
		koplocks = irix_init_kernel_oplocks();
#elif HAVE_KERNEL_OPLOCKS_LINUX
		koplocks = linux_init_kernel_oplocks();
#endif
	}

	DEBUG(3,("open_oplock ipc: pid = %d, global_oplock_port = %u\n", 
		 (int)sys_getpid(), global_oplock_port));

	return True;
}
