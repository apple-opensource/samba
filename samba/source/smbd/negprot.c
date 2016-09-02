/* 
   Unix SMB/CIFS implementation.
   negprot reply code
   Copyright (C) Andrew Tridgell 1992-1998
   
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

extern int Protocol;
extern int max_recv;
BOOL global_encrypted_passwords_negotiated = False;
BOOL global_spnego_negotiated = False;
struct auth_context *negprot_global_auth_context = NULL;

static void get_challenge(char buff[8]) 
{
	NTSTATUS nt_status;
	const uint8 *cryptkey;

	/* We might be called more than once, muliple negprots are premitted */
	if (negprot_global_auth_context) {
		DEBUG(3, ("get challenge: is this a secondary negprot?  negprot_global_auth_context is non-NULL!\n"));
		(negprot_global_auth_context->free)(&negprot_global_auth_context);
	}

	DEBUG(10, ("get challenge: creating negprot_global_auth_context\n"));
	if (!NT_STATUS_IS_OK(nt_status = make_auth_context_subsystem(&negprot_global_auth_context))) {
		DEBUG(0, ("make_auth_context_subsystem returned %s", nt_errstr(nt_status)));
		smb_panic("cannot make_negprot_global_auth_context!\n");
	}
	DEBUG(10, ("get challenge: getting challenge\n"));
	cryptkey = negprot_global_auth_context->get_ntlm_challenge(negprot_global_auth_context);
	memcpy(buff, cryptkey, 8);
}

/****************************************************************************
 Reply for the core protocol.
****************************************************************************/

static int reply_corep(char *inbuf, char *outbuf)
{
	int outsize = set_message(outbuf,1,0,True);

	Protocol = PROTOCOL_CORE;
	
	return outsize;
}

/****************************************************************************
 Reply for the coreplus protocol.
****************************************************************************/

static int reply_coreplus(char *inbuf, char *outbuf)
{
	int raw = (lp_readraw()?1:0) | (lp_writeraw()?2:0);
	int outsize = set_message(outbuf,13,0,True);
	SSVAL(outbuf,smb_vwv5,raw); /* tell redirector we support
			readbraw and writebraw (possibly) */
	/* Reply, SMBlockread, SMBwritelock supported. */
	SCVAL(outbuf,smb_flg,FLAG_REPLY|FLAG_SUPPORT_LOCKREAD);
	SSVAL(outbuf,smb_vwv1,0x1); /* user level security, don't encrypt */	

	Protocol = PROTOCOL_COREPLUS;

	return outsize;
}

/****************************************************************************
 Reply for the lanman 1.0 protocol.
****************************************************************************/

static int reply_lanman1(char *inbuf, char *outbuf)
{
	int raw = (lp_readraw()?1:0) | (lp_writeraw()?2:0);
	int secword=0;
	time_t t = time(NULL);

	global_encrypted_passwords_negotiated = lp_encrypted_passwords();

	if (lp_security()>=SEC_USER)
		secword |= NEGOTIATE_SECURITY_USER_LEVEL;
	if (global_encrypted_passwords_negotiated)
		secword |= NEGOTIATE_SECURITY_CHALLENGE_RESPONSE;

	set_message(outbuf,13,global_encrypted_passwords_negotiated?8:0,True);
	SSVAL(outbuf,smb_vwv1,secword); 
	/* Create a token value and add it to the outgoing packet. */
	if (global_encrypted_passwords_negotiated) {
		get_challenge(smb_buf(outbuf));
	}

	Protocol = PROTOCOL_LANMAN1;

	/* Reply, SMBlockread, SMBwritelock supported. */
	SCVAL(outbuf,smb_flg,FLAG_REPLY|FLAG_SUPPORT_LOCKREAD);
	SSVAL(outbuf,smb_vwv2,max_recv);
	SSVAL(outbuf,smb_vwv3,lp_maxmux()); /* maxmux */
	SSVAL(outbuf,smb_vwv4,1);
	SSVAL(outbuf,smb_vwv5,raw); /* tell redirector we support
		readbraw writebraw (possibly) */
	SIVAL(outbuf,smb_vwv6,sys_getpid());
	SSVAL(outbuf,smb_vwv10, TimeDiff(t)/60);

	put_dos_date(outbuf,smb_vwv8,t);

	return (smb_len(outbuf)+4);
}

/****************************************************************************
 Reply for the lanman 2.0 protocol.
****************************************************************************/

static int reply_lanman2(char *inbuf, char *outbuf)
{
	int raw = (lp_readraw()?1:0) | (lp_writeraw()?2:0);
	int secword=0;
	time_t t = time(NULL);

	global_encrypted_passwords_negotiated = lp_encrypted_passwords();
  
	if (lp_security()>=SEC_USER)
		secword |= NEGOTIATE_SECURITY_USER_LEVEL;
	if (global_encrypted_passwords_negotiated)
		secword |= NEGOTIATE_SECURITY_CHALLENGE_RESPONSE;

	set_message(outbuf,13,global_encrypted_passwords_negotiated?8:0,True);
	SSVAL(outbuf,smb_vwv1,secword); 
	SIVAL(outbuf,smb_vwv6,sys_getpid());

	/* Create a token value and add it to the outgoing packet. */
	if (global_encrypted_passwords_negotiated) {
		get_challenge(smb_buf(outbuf));
	}

	Protocol = PROTOCOL_LANMAN2;

	/* Reply, SMBlockread, SMBwritelock supported. */
	SCVAL(outbuf,smb_flg,FLAG_REPLY|FLAG_SUPPORT_LOCKREAD);
	SSVAL(outbuf,smb_vwv2,max_recv);
	SSVAL(outbuf,smb_vwv3,lp_maxmux()); 
	SSVAL(outbuf,smb_vwv4,1);
	SSVAL(outbuf,smb_vwv5,raw); /* readbraw and/or writebraw */
	SSVAL(outbuf,smb_vwv10, TimeDiff(t)/60);
	put_dos_date(outbuf,smb_vwv8,t);

	return (smb_len(outbuf)+4);
}

/****************************************************************************
 Generate the spnego negprot reply blob. Return the number of bytes used.
****************************************************************************/

static int negprot_spnego(char *p)
{
	DATA_BLOB blob;
	uint8 guid[16];
	const char *OIDs_krb5[] = {OID_KERBEROS5,
				   OID_KERBEROS5_OLD,
				   OID_NTLMSSP,
				   NULL};
	const char *OIDs_plain[] = {OID_NTLMSSP, NULL};
	char *principal;
	int len;

	global_spnego_negotiated = True;

	memset(guid, 0, 16);
	safe_strcpy((char *)guid, global_myname(), 16);
	strlower((char *)guid);

#if 0
	/* strangely enough, NT does not sent the single OID NTLMSSP when
	   not a ADS member, it sends no OIDs at all

	   we can't do this until we teach our sesssion setup parser to know
	   about raw NTLMSSP (clients send no ASN.1 wrapping if we do this)
	*/
	if (lp_security() != SEC_ADS) {
		memcpy(p, guid, 16);
		return 16;
	}
#endif
	if (lp_security() != SEC_ADS) {
		blob = spnego_gen_negTokenInit(guid, OIDs_plain, "NONE");
	} else {
		asprintf(&principal, "%s$@%s", guid, lp_realm());
		blob = spnego_gen_negTokenInit(guid, OIDs_krb5, principal);
		free(principal);
	}
	memcpy(p, blob.data, blob.length);
	len = blob.length;
	data_blob_free(&blob);
	return len;
}

/****************************************************************************
 Reply for the nt protocol.
****************************************************************************/

static int reply_nt1(char *inbuf, char *outbuf)
{
	/* dual names + lock_and_read + nt SMBs + remote API calls */
	int capabilities = CAP_NT_FIND|CAP_LOCK_AND_READ|
		CAP_LEVEL_II_OPLOCKS;

	int secword=0;
	time_t t = time(NULL);
	char *p, *q;
	BOOL negotiate_spnego = False;

	global_encrypted_passwords_negotiated = lp_encrypted_passwords();

	/* do spnego in user level security if the client
	   supports it and we can do encrypted passwords */
	
	if (global_encrypted_passwords_negotiated && 
	    (lp_security() != SEC_SHARE) &&
	    lp_use_spnego() &&
	    (SVAL(inbuf, smb_flg2) & FLAGS2_EXTENDED_SECURITY)) {
		negotiate_spnego = True;
		capabilities |= CAP_EXTENDED_SECURITY;
	}
	
	capabilities |= CAP_NT_SMBS|CAP_RPC_REMOTE_APIS;

	if (lp_unix_extensions()) {
		capabilities |= CAP_UNIX;
	}
	
	if (lp_large_readwrite() && (SMB_OFF_T_BITS == 64))
		capabilities |= CAP_LARGE_READX|CAP_LARGE_WRITEX|CAP_W2K_SMBS;
	
	if (SMB_OFF_T_BITS == 64)
		capabilities |= CAP_LARGE_FILES;

	if (lp_readraw() && lp_writeraw())
		capabilities |= CAP_RAW_MODE;
	
	/* allow for disabling unicode */
	if (lp_unicode())
		capabilities |= CAP_UNICODE;

	if (lp_nt_status_support())
		capabilities |= CAP_STATUS32;
	
	if (lp_host_msdfs())
		capabilities |= CAP_DFS;
	
	if (lp_security() >= SEC_USER)
		secword |= NEGOTIATE_SECURITY_USER_LEVEL;
	if (global_encrypted_passwords_negotiated)
		secword |= NEGOTIATE_SECURITY_CHALLENGE_RESPONSE;
	
	set_message(outbuf,17,0,True);
	
	SCVAL(outbuf,smb_vwv1,secword);
	
	Protocol = PROTOCOL_NT1;
	
	SSVAL(outbuf,smb_vwv1+1,lp_maxmux()); /* maxmpx */
	SSVAL(outbuf,smb_vwv2+1,1); /* num vcs */
	SIVAL(outbuf,smb_vwv3+1,max_recv); /* max buffer. LOTS! */
	SIVAL(outbuf,smb_vwv5+1,0x10000); /* raw size. full 64k */
	SIVAL(outbuf,smb_vwv7+1,sys_getpid()); /* session key */
	SIVAL(outbuf,smb_vwv9+1,capabilities); /* capabilities */
	put_long_date(outbuf+smb_vwv11+1,t);
	SSVALS(outbuf,smb_vwv15+1,TimeDiff(t)/60);
	
	p = q = smb_buf(outbuf);
	if (!negotiate_spnego) {
		/* Create a token value and add it to the outgoing packet. */
		if (global_encrypted_passwords_negotiated) {
			/* note that we do not send a challenge at all if
			   we are using plaintext */
			get_challenge(p);
			SSVALS(outbuf,smb_vwv16+1,8);
			p += 8;
		}
		p += srvstr_push(outbuf, p, lp_workgroup(), -1, 
				 STR_UNICODE|STR_TERMINATE|STR_NOALIGN);
		DEBUG(3,("not using SPNEGO\n"));
	} else {
		int len = negprot_spnego(p);
		
		SSVALS(outbuf,smb_vwv16+1,len);
		p += len;
		DEBUG(3,("using SPNEGO\n"));
	}
	
	SSVAL(outbuf,smb_vwv17, p - q); /* length of challenge+domain strings */
	set_message_end(outbuf, p);
	
	return (smb_len(outbuf)+4);
}

/* these are the protocol lists used for auto architecture detection:

WinNT 3.51:
protocol [PC NETWORK PROGRAM 1.0]
protocol [XENIX CORE]
protocol [MICROSOFT NETWORKS 1.03]
protocol [LANMAN1.0]
protocol [Windows for Workgroups 3.1a]
protocol [LM1.2X002]
protocol [LANMAN2.1]
protocol [NT LM 0.12]

Win95:
protocol [PC NETWORK PROGRAM 1.0]
protocol [XENIX CORE]
protocol [MICROSOFT NETWORKS 1.03]
protocol [LANMAN1.0]
protocol [Windows for Workgroups 3.1a]
protocol [LM1.2X002]
protocol [LANMAN2.1]
protocol [NT LM 0.12]

Win2K:
protocol [PC NETWORK PROGRAM 1.0]
protocol [LANMAN1.0]
protocol [Windows for Workgroups 3.1a]
protocol [LM1.2X002]
protocol [LANMAN2.1]
protocol [NT LM 0.12]

OS/2:
protocol [PC NETWORK PROGRAM 1.0]
protocol [XENIX CORE]
protocol [LANMAN1.0]
protocol [LM1.2X002]
protocol [LANMAN2.1]
*/

/*
  * Modified to recognize the architecture of the remote machine better.
  *
  * This appears to be the matrix of which protocol is used by which
  * MS product.
       Protocol                       WfWg    Win95   WinNT  Win2K  OS/2
       PC NETWORK PROGRAM 1.0          1       1       1      1      1
       XENIX CORE                                      2             2
       MICROSOFT NETWORKS 3.0          2       2       
       DOS LM1.2X002                   3       3       
       MICROSOFT NETWORKS 1.03                         3
       DOS LANMAN2.1                   4       4       
       LANMAN1.0                                       4      2      3
       Windows for Workgroups 3.1a     5       5       5      3
       LM1.2X002                                       6      4      4
       LANMAN2.1                                       7      5      5
       NT LM 0.12                              6       8      6
  *
  *  tim@fsg.com 09/29/95
  *  Win2K added by matty 17/7/99
  */
  
#define ARCH_WFWG     0x3      /* This is a fudge because WfWg is like Win95 */
#define ARCH_WIN95    0x2
#define ARCH_WINNT    0x4
#define ARCH_WIN2K    0xC      /* Win2K is like NT */
#define ARCH_OS2      0x14     /* Again OS/2 is like NT */
#define ARCH_SAMBA    0x20
 
#define ARCH_ALL      0x3F
 
/* List of supported protocols, most desired first */
static const struct {
	const char *proto_name;
	const char *short_name;
	int (*proto_reply_fn)(char *, char *);
	int protocol_level;
} supported_protocols[] = {
	{"NT LANMAN 1.0",           "NT1",      reply_nt1,      PROTOCOL_NT1},
	{"NT LM 0.12",              "NT1",      reply_nt1,      PROTOCOL_NT1},
	{"LM1.2X002",               "LANMAN2",  reply_lanman2,  PROTOCOL_LANMAN2},
	{"Samba",                   "LANMAN2",  reply_lanman2,  PROTOCOL_LANMAN2},
	{"DOS LM1.2X002",           "LANMAN2",  reply_lanman2,  PROTOCOL_LANMAN2},
	{"LANMAN1.0",               "LANMAN1",  reply_lanman1,  PROTOCOL_LANMAN1},
	{"MICROSOFT NETWORKS 3.0",  "LANMAN1",  reply_lanman1,  PROTOCOL_LANMAN1},
	{"MICROSOFT NETWORKS 1.03", "COREPLUS", reply_coreplus, PROTOCOL_COREPLUS},
	{"PC NETWORK PROGRAM 1.0",  "CORE",     reply_corep,    PROTOCOL_CORE}, 
	{NULL,NULL,NULL,0},
};

/****************************************************************************
 Reply to a negprot.
****************************************************************************/

int reply_negprot(connection_struct *conn, 
		  char *inbuf,char *outbuf, int dum_size, 
		  int dum_buffsize)
{
	int outsize = set_message(outbuf,1,0,True);
	int Index=0;
	int choice= -1;
	int protocol;
	char *p;
	int bcc = SVAL(smb_buf(inbuf),-2);
	int arch = ARCH_ALL;

	static BOOL done_negprot = False;

	START_PROFILE(SMBnegprot);

	if (done_negprot) {
		END_PROFILE(SMBnegprot);
		exit_server("multiple negprot's are not permitted");
	}
	done_negprot = True;

	p = smb_buf(inbuf)+1;
	while (p < (smb_buf(inbuf) + bcc)) { 
		Index++;
		DEBUG(3,("Requested protocol [%s]\n",p));
		if (strcsequal(p,"Windows for Workgroups 3.1a"))
			arch &= ( ARCH_WFWG | ARCH_WIN95 | ARCH_WINNT | ARCH_WIN2K );
		else if (strcsequal(p,"DOS LM1.2X002"))
			arch &= ( ARCH_WFWG | ARCH_WIN95 );
		else if (strcsequal(p,"DOS LANMAN2.1"))
			arch &= ( ARCH_WFWG | ARCH_WIN95 );
		else if (strcsequal(p,"NT LM 0.12"))
			arch &= ( ARCH_WIN95 | ARCH_WINNT | ARCH_WIN2K );
		else if (strcsequal(p,"LANMAN2.1"))
			arch &= ( ARCH_WINNT | ARCH_WIN2K | ARCH_OS2 );
		else if (strcsequal(p,"LM1.2X002"))
			arch &= ( ARCH_WINNT | ARCH_WIN2K | ARCH_OS2 );
		else if (strcsequal(p,"MICROSOFT NETWORKS 1.03"))
			arch &= ARCH_WINNT;
		else if (strcsequal(p,"XENIX CORE"))
			arch &= ( ARCH_WINNT | ARCH_OS2 );
		else if (strcsequal(p,"Samba")) {
			arch = ARCH_SAMBA;
			break;
		}
 
		p += strlen(p) + 2;
	}
    
	switch ( arch ) {
		case ARCH_SAMBA:
			set_remote_arch(RA_SAMBA);
			break;
		case ARCH_WFWG:
			set_remote_arch(RA_WFWG);
			break;
		case ARCH_WIN95:
			set_remote_arch(RA_WIN95);
			break;
		case ARCH_WINNT:
			if(SVAL(inbuf,smb_flg2)==FLAGS2_WIN2K_SIGNATURE)
				set_remote_arch(RA_WIN2K);
			else
				set_remote_arch(RA_WINNT);
			break;
		case ARCH_WIN2K:
			set_remote_arch(RA_WIN2K);
			break;
		case ARCH_OS2:
			set_remote_arch(RA_OS2);
			break;
		default:
			set_remote_arch(RA_UNKNOWN);
		break;
	}
 
	/* possibly reload - change of architecture */
	reload_services(True);      
    
	/* Check for protocols, most desirable first */
	for (protocol = 0; supported_protocols[protocol].proto_name; protocol++) {
		p = smb_buf(inbuf)+1;
		Index = 0;
		if ((supported_protocols[protocol].protocol_level <= lp_maxprotocol()) &&
				(supported_protocols[protocol].protocol_level >= lp_minprotocol()))
			while (p < (smb_buf(inbuf) + bcc)) { 
				if (strequal(p,supported_protocols[protocol].proto_name))
					choice = Index;
				Index++;
				p += strlen(p) + 2;
			}
		if(choice != -1)
			break;
	}
  
	SSVAL(outbuf,smb_vwv0,choice);
	if(choice != -1) {
		extern fstring remote_proto;
		fstrcpy(remote_proto,supported_protocols[protocol].short_name);
		reload_services(True);          
		outsize = supported_protocols[protocol].proto_reply_fn(inbuf, outbuf);
		DEBUG(3,("Selected protocol %s\n",supported_protocols[protocol].proto_name));
	} else {
		DEBUG(0,("No protocol supported !\n"));
	}
	SSVAL(outbuf,smb_vwv0,choice);
  
	DEBUG( 5, ( "negprot index=%d\n", choice ) );

	END_PROFILE(SMBnegprot);
	return(outsize);
}
