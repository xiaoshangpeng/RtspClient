#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <netdb.h>
#include <sys/socket.h>
#include <errno.h>
#include <stdarg.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <fcntl.h> 
#include <pthread.h>
#include <sys/ipc.h> 
#include <sys/msg.h>
#include <netinet/if_ether.h>
#include <net/if.h>

#include <linux/if_ether.h>
#include <linux/sockios.h>
#include <netinet/in.h> 
#include <arpa/inet.h> 

#define OUR_DEV
typedef void * HANDLE;
#ifdef OUR_DEV
#define DEF_RTSP_URL "rtsp://192.168.2.101/test10.264"
#else
#define DEF_RTSP_URL "rtsp://192.168.0.29/"
#endif

#define RTSP_PORT 554
#define PRG_NAME	"main"
#define UDP_RECV_PORT0 	1023
#define UDP_RECV_PORT1 	1024

typedef enum RTSP_STAT
{
	RTSP_NONE = 0,
	RTSP_OPTION,
	RTSP_DESCRIPT,
	RTSP_SETUP,
	RTSP_PLAY,
	RTSP_GETPARM,
	RTSP_KEEP,
	RTSP_PAUSE,
	RTSP_STOP
}RTPS_STAT_E;

typedef enum LINK_STAT
{
	LINK_DISCONNECT = 0,
	LINK_CONNECTING,
	LINK_CONNECTED
}LINK_STAT_E;

typedef enum TRANSPORT
{
	TRANS_UDP = 0,
	TRANS_TCP,
	TRANS_RAW
}TRANSPORT_E;

typedef struct tagRtspClient
{
	unsigned int magic;
	int fd; 	//  rtsp web socket
	TRANSPORT_E stream_type;	// 码流方式
	int recv_fd[2]; //
	int recv_port[2]; // 监听端口
	char rtsp_url[128];
	char session[20];
	char track_addr[128]; 
	char authName[64];
	char authPwd[64];
	int support_cmd;
	int bQuit;
	int trackId;
	time_t tou;
	RTPS_STAT_E stat;
	LINK_STAT_E link;
	int CSeq;
	void *recv_buf;
	int maxBufSize;
	HANDLE *Filter;
	HANDLE *Vdec;
//	struct list_head list;
}RtspClient, *PVHRtspClient;


#define SOCK_ERROR() do{\
	if( len <= 0 )\
	{\
		fd = 0;\
		printf("sock error, %s!\r\n", strerror(errno));\
		break;\
	}\
}while(0)

/*  TCP and UDP API*/

int sock_listen( int port, const char *ipbind, int backlog )
{
	struct sockaddr_in 	my_addr;
	int 			fd, tmp = 1;

	fd = socket(AF_INET,SOCK_STREAM,0);
	if (fd < 0) 
  	 return -1;

 setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &tmp, sizeof(tmp));

	memset(&my_addr, 0, sizeof(my_addr));
  my_addr.sin_family = AF_INET;
  my_addr.sin_port = htons ((short)port);
	if( ipbind != NULL ) {
		inet_aton( ipbind, & my_addr.sin_addr );
	} else {
  	my_addr.sin_addr.s_addr = htonl (INADDR_ANY);
	}

	if( 0 == bind (fd, (struct sockaddr *) &my_addr, sizeof (my_addr)) )
	{
		if( 0 == listen(fd, backlog) ) {
			return fd;
		}
	}

	close(fd);
	return -1;
}


int sock_dataready( int fd, int tout )
{
	fd_set	rfd_set;
	struct	timeval tv, *ptv;
	int	nsel;

	FD_ZERO( &rfd_set );
	FD_SET( fd, &rfd_set );
	if ( tout == -1 )
	{
		ptv = NULL;
	}
	else
	{
		tv.tv_sec = 0;
		tv.tv_usec = tout * 1000;
		ptv = &tv;
	}
	nsel = select( fd+1, &rfd_set, NULL, NULL, ptv );
	if ( nsel > 0 && FD_ISSET( fd, &rfd_set ) )
		return 1;
	return 0;
}


int sock_udp_bind( int port )
{
	struct sockaddr_in 	my_addr;
	int			tmp=0;
	int			udp_fd;

	// signal init, to avoid app quit while pipe broken
//	signal(SIGPIPE, SIG_IGN);

	if ( (udp_fd = socket( AF_INET, SOCK_DGRAM, 0 )) >= 0 )
	{
		setsockopt( udp_fd, SOL_SOCKET, SO_REUSEADDR, &tmp, sizeof(tmp));

		memset(&my_addr, 0, sizeof(my_addr));
		my_addr.sin_family = AF_INET;
		my_addr.sin_port = htons ((short)port);
		my_addr.sin_addr.s_addr = htonl (INADDR_ANY); // get_ifadapter_ip("eth0",NULL);

		if (bind ( udp_fd, (struct sockaddr *) &my_addr, sizeof (my_addr)) < 0)
		{
    	close( udp_fd );
    	udp_fd = -EIO;
  	}
	}
  return udp_fd;
}

static sock_read( int fd , char *buf, int maxBuf )
{
	return read( fd, buf, maxBuf );
}

int sock_connect( const char *host, int port )
{
	struct sockaddr_in	destaddr;
  struct hostent 		*hp;
	int 			fd = 0;

	memset( & destaddr, 0, sizeof(destaddr) );
	destaddr.sin_family = AF_INET;
	destaddr.sin_port = htons( (short)port );
  if ((inet_aton(host, & destaddr.sin_addr)) == 0)
  {
      hp = gethostbyname(host);
      if(! hp) return -EINVAL;
      memcpy (& destaddr.sin_addr, hp->h_addr, sizeof(destaddr.sin_addr));
  }

	fd = socket(PF_INET, SOCK_STREAM, 0);
	if (fd < 0)   return -EIO;

	if ( connect(fd, (struct sockaddr *)&destaddr, sizeof(destaddr)) < 0 )
	{
		close( fd );
		return -EIO;
	}
	return fd;
}
		
static const char* to_b64 =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	"abcdefghijklmnopqrstuvwxyz"
	"0123456789+/";

/* encode 72 characters	per	line */
#define	CHARS_PER_LINE	72

typedef unsigned char	byte;

/* return value:
 * >0: encoded string length, -1 buf too small
 * Encoded string is terminated by '\0'.
 */
int str_b64enc(const char *src, char *buf, int bufsize )
{
	int	size	= strlen( src );
	int	div	= size / 3;
	int	rem	= size % 3;
	int	chars 	= div*4 + rem + 1;
	int	newlines = (chars + CHARS_PER_LINE - 1)	/ CHARS_PER_LINE;
	int	outsize  = chars + newlines;
	const byte*	data = (const byte *)src;
	byte*		enc = (byte *)buf;

	if ( bufsize < outsize + 1 ) return -1;
	chars =	0;
	while (div > 0)
	{
		enc[0] = to_b64[ (data[0] >> 2)	& 0x3f];
		enc[1] = to_b64[((data[0] << 4)	& 0x30)	+ ((data[1] >> 4) & 0xf)];
		enc[2] = to_b64[((data[1] << 2)	& 0x3c)	+ ((data[2] >> 6) & 0x3)];
		enc[3] = to_b64[ data[2] & 0x3f];
		data +=	3;
		enc  += 4;
		div--;
		chars += 4;
		if (chars == CHARS_PER_LINE)
		{
			chars =	0;
//			*(enc++) = '\n';	/* keep the encoded string in single line */
		}
	}

	switch (rem)
	{
		case 2:
			enc[0] = to_b64[ (data[0] >> 2)	& 0x3f];
			enc[1] = to_b64[((data[0] << 4)	& 0x30)	+ ((data[1] >> 4) & 0xf)];
			enc[2] = to_b64[ (data[1] << 2)	& 0x3c];
			enc[3] = '=';
			enc   += 4;
			chars += 4;
			break;
		case 1:
			enc[0] = to_b64[ (data[0] >> 2)	& 0x3f];
			enc[1] = to_b64[ (data[0] << 4)	& 0x30];
			enc[2] = '=';
			enc[3] = '=';
			enc   += 4;
			chars += 4;
			break;
	}

	*enc = '\0';
	return strlen(buf);		// exclude the tail '\0'
}

/*
 * decode a base64 encoded string.
 * return -1: bufsize too small, \
 *         0: string content error,
 *       > 0: decoded string (null terminated).
 */
int  str_b64dec(const char* string, char *buf, int bufsize)
{
	register int length = string ? strlen(string) : 0;
	register byte* data = (byte *)buf;

	/* do a	format verification first */
	if (length > 0)
	{
		register int count = 0,	rem	= 0;
		register const char* tmp = string;

		while (length >	0)
		{
			register int skip = strspn(tmp,	to_b64);
			count += skip;
			length -= skip;
			tmp += skip;
			if (length > 0)
			{
				register int i,	vrfy = strcspn(tmp, to_b64);

				for (i = 0; i < vrfy; i++)
				{
					if (isspace(tmp[i])) continue;
					if (tmp[i] == '=')
					{
						/* we should check if we're close to the end of	the string */
						if ( (rem = count % 4) < 2 )
							/* rem must be either 2	or 3, otherwise	no '=' should be here */
							return 0;
						/* end-of-message recognized */
						break;
					}
					else
					{
						/* Invalid padding character. */
						return 0;
					}
				}
				length -= vrfy;
				tmp    += vrfy;
			}
		}
		if ( bufsize < (count/4 * 3 + (rem ? (rem-1) : 0)) )
			return -1;

		if (count > 0)
		{
			register int i,	qw = 0;

			length = strlen(string);
			for (i = 0; i < length; i++)
			{
			register char ch = string[i];
				register byte bits;

				if (isspace(ch)) continue;

				bits = 0;
				if ((ch	>= 'A')	&& (ch <= 'Z'))
				{
					bits = (byte) (ch - 'A');
				}
				else if	((ch >=	'a') &&	(ch <= 'z'))
				{
					bits = (byte) (ch - 'a'	+ 26);
				}
				else if	((ch >=	'0') &&	(ch <= '9'))
				{
					bits = (byte) (ch - '0'	+ 52);
				}
				else if	(ch == '=')
					break;

				switch (qw++)
				{
					case 0:
						data[0] = (bits << 2) & 0xfc;
						break;
					case 1:
						data[0] |= (bits >> 4) & 0x03;
						data[1] = (bits << 4) & 0xf0;
						break;
					case 2:
						data[1] |= (bits >> 2) & 0x0f;
						data[2] = (bits << 6) & 0xc0;
						break;
					case 3:
						data[2] |= bits & 0x3f;
						break;
				}
				if (qw == 4)
				{
					qw = 0;
					data += 3;
				}
			}
			data += qw;
			*data = '\0';
		}
	}
	return data - (unsigned char *)buf;
}

int strstartwith(const char *string, const char *prefix, int minmatch, char ** pleft)
{
	int len = strlen(prefix);
	int n = strlen( string );
	if ( n < len && n >=minmatch && minmatch ) len = n;
	
	if( 0 == strncmp(string, prefix, len) ) {
		if(pleft) *pleft = (char*)(string + len);
		return 1;
	}
	if(pleft) *pleft = NULL;
	return 0;
}

int stridxinargs( const char *key, int minmatch,...)
{
	va_list va;
	char *matcharg;
	int  matched_arg = -1;
	int arg_index = 0;
	
	va_start( va, minmatch );
	while ( (matcharg = va_arg( va , char * )) != NULL )
	{
		if ( (minmatch==0 && strcmp(key, matcharg)==0) ||
		    (minmatch && strstartwith( key, matcharg, minmatch, NULL) ) )
		{
			matched_arg = arg_index;
			break;
		}
		arg_index++;
	}
	va_end(va);
	return matched_arg;
}

int strgetword( const char *string, char *buf, int size, char **pleft)
{
	const char *ptr, *q;
	int len;

	/* skip leading white-space */
	for(ptr=string; *ptr && (*ptr==' ' || *ptr=='\t' || *ptr=='\n'); ptr++);
	if ( *ptr=='\0' ) return 0;
	for(q=ptr; *q && *q!=' ' && *q!='\t' && *q!='\n'; q++);
	len = q - ptr;
	if ( len < size )
	{
		memcpy( buf, ptr, len );
		buf[len] = '\0';
		if ( pleft )
			*pleft=(char*)q;
		return len;
	}
	return 0;
}



/* TCP and UDP API */

static const char *read_line( char *buf, char *line, char **dptr )
{
	if( !buf  || *buf == '\0') return;
	char *ptr = buf;
	char *lineS = line;
	for( ;*ptr != '\n'; ptr++ ) *line++ = *ptr;
	*line++ = '\n';
	*line = '\0';
	*dptr = (ptr+1);
	return lineS;
}

static int new_sock( int type, PVHRtspClient client )
{
	int i = 0;
	client->stream_type = TRANS_TCP;
	if( type == 0 )
	{
		client->stream_type = TRANS_UDP;
		printf("使用UDP方式获取码流!\r\n");
	}
	else
		printf("使用TCP方式获取码流!\r\n");
	int fd0, fd1;
	if( client->recv_fd[0] > 0 )
		close( client->recv_fd[0] );
	if( client->recv_fd[1] > 0 )
		close( client->recv_fd[1] );
	client->recv_port[0] = 0;
	client->recv_port[1] = 0;
	for( i; i < 100; i++ )
	{
		if( client->stream_type == TRANS_UDP )
		{
			fd0 = sock_udp_bind( 1024 + i );
			if( fd0 < 0 )
				continue;
			fd1 = sock_udp_bind( 1024 + i+1 );
			if( fd1 < 0 )
			{
				i++;
				close( fd0 );
				continue;
			}
			client->recv_fd[0] = fd0;
			client->recv_fd[1] = fd1;
			client->recv_port[0] = 1024 + i;
			client->recv_port[1] = 1024 + i+1;
			printf("绑定本地UDP端口:%d -%d \r\n", client->recv_port[0], client->recv_port[1] );
			break;
		}else if( client->stream_type == TRANS_TCP ){
			fd0 = sock_listen(1024 + i, NULL, 0);
			if( fd0 < 0 )
				continue;
			fd1 = sock_listen(1024 + i + 1, NULL, 0);
			if( fd1 < 0 )
			{
				i++;
				close( fd0 );
				continue;
			}
			client->recv_fd[0] = fd0;
			client->recv_fd[1] = fd1;
			client->recv_port[0] = 1024 + i;
			client->recv_port[1] = 1024 + i+1;
			break;
		}	
	}
	if( client->recv_port[0] > 0 )
		return 0;
	else
		return -1;
}


static int IsAnsOK( char *buf )
{
	int code = 0;
	if( buf == NULL )
		return -1;
	sscanf( buf, "RTSP/1.0 %d", &code );
	if( code == 200 )
		return 0;
	printf("ANS ERROR:%s \r\n", buf );
	return -1;
}

static void AnsOption( HANDLE handle, char *buf )
{
	PVHRtspClient obj = (PVHRtspClient)handle;
	obj->support_cmd = 0;
	if( IsAnsOK( buf ) != 0 )
		return;
	char *ptr = NULL;
	ptr = strstr( buf, "\r\n\r\n" );
	if( ptr )
	{
		ptr += 4;
		if( *ptr == '\0' )
			return;
		printf("Content:%s\r\n", ptr);
	}
	char *options = NULL;
	char key[30];
	while( strgetword( options, key, sizeof( key ), &options ) != 0 )
	{
		int index = stridxinargs( key, 5, "OPTIONS", "DESCRIBE", "SETUP", "PLAY", "TEARDOWN", "GET_PARAMETER" );
		switch( index )
		{
			case 0:  obj->support_cmd |= (1)<<0;  break;
			case 1:  obj->support_cmd |= (1)<<1;  break;
			case 2:  obj->support_cmd |= (1)<<2;  break;
			case 3:  obj->support_cmd |= (1)<<3; break;
			case 4:  obj->support_cmd |= (1)<<4;  break;
			case 5:  obj->support_cmd |= (1)<<5;  break;
			case 6:  obj->support_cmd |= (1)<<6;  break;
			default:
				 break;
		}
	}


	printf("Let's go to next get descript!\r\n" );
	obj->stat = RTSP_DESCRIPT;	
}

static void AnsDescript( HANDLE handle, char *buf )
{
	PVHRtspClient obj = (PVHRtspClient)handle;
	if( IsAnsOK( buf ) != 0 )
		return;
	char *ptr = NULL;
	ptr = strstr( buf, "\r\n\r\n" );
	if( ptr )
	{
		ptr += 4;
		if( *ptr == '\0' )
			return;
	}
	printf("Let's go to next get setup!\r\n" );
	obj->stat = RTSP_SETUP;	
}

static void AnsSetup( HANDLE handle, char *buf )
{
	PVHRtspClient obj = (PVHRtspClient)handle;
	if( IsAnsOK( buf ) != 0 )
		return;
	char session[128];
	char *ptr, *q;
	q = session;
	ptr = strstr( buf, "Session" );
	if( ptr )
	{
		while( *ptr != '\r' && *ptr != ';' && *ptr != '\t' && *ptr != '\n')
			*q++ = *ptr++;
		*q = '\0';
		strcpy( obj->session, session + 9 );
		printf("========>>>>>>> Get Sesson:%s \r\n", obj->session );
	}
	obj->stat = RTSP_PLAY;	
}

static void AnsPlay( HANDLE handle, char *buf )
{
	PVHRtspClient obj = (PVHRtspClient)handle;
	if( IsAnsOK( buf ) != 0 )
		return;
	char *ptr = NULL;
	ptr = strstr( buf, "\r\n\r\n" );
	if( ptr )
	{
		ptr += 4;
		if( *ptr == '\0' )
			return;
	}
	obj->stat = RTSP_KEEP;	
}

static void AnsGetParam( HANDLE handle, char *buf )
{
	PVHRtspClient obj = (PVHRtspClient)handle;
	if( IsAnsOK( buf ) != 0 )
		return;
	char *ptr = NULL;
	ptr = strstr( buf, "\r\n\r\n" );
	if( ptr )
	{
		ptr += 4;
		if( *ptr == '\0' )
			return;
		printf("Content:%s\r\n", ptr);
	}
	obj->stat = RTSP_KEEP;	
}

static void AnsStop( HANDLE handle, char *buf )
{
	if( IsAnsOK( buf ) != 0 )
		return;
	char *ptr = NULL;
	ptr = strstr( buf, "\r\n\r\n" );
	if( ptr )
	{
		ptr += 4;
		if( *ptr == '\0' )
			return;
		printf("Content:%s\r\n", ptr);
	}
}

static const char *getAuthurationInfo( HANDLE handle )
{
	//这边的内容待定，暂时没有用户名密码输入的需求，后期有的话再开发，大概这这么写的
	PVHRtspClient obj = (PVHRtspClient)handle;	
	static char authon[256] = {0};
	if( obj->authName[0] == '\0')
	{
		memset(authon, 0, sizeof( authon ) );
		return authon;
	}
	char body[128] = {0};
	char enBody[256] = {0};
	char const* const authFmt = "Authorization: Basic %s\r\n";
	unsigned usernamePasswordLength = strlen(obj->authName) + 1 + strlen(obj->authPwd);
	sprintf(body, "%s:%s", obj->authName, obj->authPwd);
	int len = str_b64enc(body, enBody, sizeof( enBody ));
//	unsigned const authBufSize = strlen(authFmt) + strlen(enBody) + 1;
	sprintf(authon, authFmt, enBody );
	return authon;
}

int SendRequest( HANDLE handle , RTPS_STAT_E type )
{
	PVHRtspClient obj = (PVHRtspClient)handle;
	char *cmd = NULL;
	int CSeq = obj->CSeq++;
	char Agent[128] = {0}, StrCSeq[30] = {0}, Authoration[128] = {0};
	char contentLengthHeader[128] = {0}, extraHeaders[128] = {0};
	char contentStr[512] = {0};
	char const* protocolStr = "RTSP/1.0"; // by default
	char const* const cmdFmt =
	"%s %s %s\r\n"	//  type, url, rtsp
	"%s"	// CSeq
	"%s"	// Authuration
	"%s"	// Agent 
	"%s"	// extraHeader
	"%s"	//  contentlen
	"\r\n"
	"%s"; // content
	switch( type )
	{
	case RTSP_NONE:
	case RTSP_OPTION:
		cmd = "OPTIONS";
		break;
	case RTSP_DESCRIPT:
		cmd = "DESCRIBE";
		break;
	case RTSP_SETUP:
		cmd = "SETUP";
		break;
	case RTSP_PLAY:
		cmd = "PLAY";
		break;
	case RTSP_GETPARM:
		cmd = "GET_PARAMETER";
		break;
	case RTSP_PAUSE:
	case RTSP_STOP:
		cmd = "TEARDOWN";
		break;
	case RTSP_KEEP:
		return;
	break;
		default:
	break;
	}
	sprintf( Agent, "User-Agent: %s\r\n",  PRG_NAME );
	sprintf( StrCSeq, "CSeq: %d\r\n", CSeq );
	if( obj->session[0] != '\0' &&  obj->stat >  RTSP_SETUP )
		sprintf( extraHeaders, "Session: %s\r\nRange: npt=0.000-\r\n", obj->session );
	else  
		sprintf( extraHeaders, "Range: npt=0.000-\r\n" );
	if(  obj->stat == RTSP_SETUP )
	{
		new_sock( TRANS_UDP, obj );
		sprintf( extraHeaders, "Transport: RTP/AVP;unicast;client_port=%d-%d\r\n", obj->recv_port[0], obj->recv_port[1] );
	}
	if( obj->stat <= RTSP_DESCRIPT )
		sprintf( extraHeaders,"Accept: application/sdp\r\n");
	// if we have contelen and content info 
	sprintf( Authoration, "%s", getAuthurationInfo(obj));
	sprintf( obj->recv_buf , cmdFmt, cmd, obj->rtsp_url, protocolStr,\
	StrCSeq, Authoration, Agent, extraHeaders,\
	contentLengthHeader, contentStr );
	if( obj->fd > 0 )
	{
		printf("Send Requtst:%s \r\n", obj->recv_buf );
		write( obj->fd, obj->recv_buf, strlen( obj->recv_buf) );
	}	
	if( sock_dataready(obj->fd, 2000) )
	{
		// if we have receive data
		if( 0 >=  read( obj->fd, obj->recv_buf, obj->maxBufSize ) )
		{
			printf("No response!\r\n");
			return;
		}
		printf("ANS:\n%s\r\n", obj->recv_buf );
		switch( type )
		{
		case RTSP_NONE:
		case RTSP_OPTION:
			 AnsOption( obj,  obj->recv_buf );
			 break;
		case RTSP_DESCRIPT:
			 AnsDescript( obj,  obj->recv_buf );
			 break;
		case RTSP_SETUP:
			 AnsSetup( obj,  obj->recv_buf );
			 break;
		case RTSP_PLAY:
			 AnsPlay( obj,  obj->recv_buf );
			 break;
		case RTSP_GETPARM:
			 AnsGetParam( obj,  obj->recv_buf );
			 break;
		case RTSP_PAUSE:
		case RTSP_STOP:
			 AnsStop( obj,  obj->recv_buf );
			 break;
		break;
		 default: 
			break;
		}
	}
	return 0;
}
// extern HANDLE NewFilter( int size , HANDLE Vde);
extern int FilterWrite( HANDLE h, char *buf, int size );
const char *show_hex( const char *ch, int rlen)
{
	int i = 0;
	char *ptr =(char*)ch;
	static char buf[1024];
	memset( buf, 0, sizeof( buf ));
	char *off = buf;
	unsigned char val;
	int len = rlen > 300 ? 300 : rlen;
	memset( buf, 0, sizeof( buf ));
	for (i = 0; i < len; i++)
	{
		val = *ptr++;
		sprintf( off, "%02x ", val); 
		off+=3;
	}
	*off = '\0';
	return buf;
}

void RTCP_PackParse( PVHRtspClient client , char *buf, int size )
{

}


void RTP_PackParse( PVHRtspClient client , char *buf, int size )
{
	// 解析码流数据，存放到解码通道
//	Hi264DecFrame( client->Vdec, buf + 36, size - 36 );	
	if( ParseRtp( buf, size ) == 1 )
	{
		SaveAs();
	}

#if 0	
	if( size > 36 )
		FilterWrite( client->Filter, buf + 36, size - 36 );
#endif
}

void *rtsp_work_thread(void *args)
{
	PVHRtspClient obj = (PVHRtspClient)args;
	obj->link = LINK_CONNECTING;
	int len;	
	int fd;
	while( !obj->bQuit )
	{
		if( obj->link != LINK_CONNECTED )
			obj->link = LINK_CONNECTING;
		obj->stat = RTSP_NONE;
		if(obj->link != LINK_CONNECTED && obj->stat == RTSP_NONE )
		{
#ifdef OUR_DEV
			fd = sock_connect( "192.168.2.101", 554 );
#else
			fd = sock_connect( "192.168.0.29", 554 );
#endif
			if( fd < 0 )
			{
				printf("connect failed! %s \r\n", strerror( errno ));
				usleep( 1000*1000 );
				obj->link = LINK_DISCONNECT;
				continue;
			}else{
				printf("connect success!\r\n");
				obj->link = LINK_CONNECTED;
			}
		}
		obj->fd = fd;
		if( SendRequest(obj, RTSP_OPTION ) == 0 )
		{
			
			printf("====> option OK!\r\n");
		}else
			continue;
		if( SendRequest(obj, RTSP_DESCRIPT ) == 0 )
		{
			printf("====> Descriput OK!\r\n");
			
		}else
			continue;
		if( SendRequest(obj, RTSP_SETUP ) == 0 )
		{
			
			printf("====> Setup OK!\r\n");
		}else
			continue;
		if( SendRequest(obj, RTSP_PLAY ) == 0 )
		{
			obj->tou = time(NULL) + 60;
		}else
			continue;
		
		while( obj->stat == RTSP_KEEP )
		{
			if( obj->tou < time(NULL) )
			{
				if( SendRequest(obj, RTSP_PLAY ) == 0 )
				{	
					// each 60s should be reconnect to rtsp send play as the heartbeat
					obj->tou = time(NULL) + 60;
				}else{
					printf("===>>> Send Play Error!\r\n");
					obj->stat = RTSP_NONE;
				}
			}
			if( obj->stream_type == TRANS_TCP )
			{
				// tcp方式的话直接解析就好了，已经包含了RTP包了
				
			}else if( obj->stream_type == TRANS_UDP )
			{	
				// 如果是UDP方式，则直接接收码流存放到解码通道		
				if( sock_dataready( obj->recv_fd[1] , 20) )
				{
					len = sock_read( obj->recv_fd[1], obj->recv_buf, obj->maxBufSize );
					if( len > 4 )
						RTCP_PackParse( obj, obj->recv_buf, len );
				}
				if( sock_dataready( obj->recv_fd[0], 20) )
				{
					len = sock_read( obj->recv_fd[0], obj->recv_buf, obj->maxBufSize );
					if( len > 4 )
						RTP_PackParse( obj, obj->recv_buf, len );
				}
			}
		}
	}
	close( obj->fd );
}

void receive_udp()
{

	int fd0 = sock_udp_bind( UDP_RECV_PORT0 );
	int fd1 = sock_udp_bind( UDP_RECV_PORT1 );
	printf("Fd:%d Fd:%d \r\n", fd0, fd1 );
	int len;
	char recv_buf[4096];
	int maxBufSize = 4096;
	while( 1 )
	{
			if( sock_dataready( fd0 , 200) )
			{
				len = sock_read( fd0, recv_buf, maxBufSize );
				RTCP_PackParse( NULL, recv_buf, len );
				printf("udp has data incomming!:%d \r\n", len );
			}
				if( sock_dataready( fd1, 200) )
			{
				len = sock_read( fd1, recv_buf, maxBufSize );
				RTP_PackParse( NULL, recv_buf, len );
				printf("udp has data incomming!:%d \r\n", len );
			}
	}
}

#define URL
int main(int argc, char *argv[])
{
	RTP_Init();
	RtspClient	*client = malloc( sizeof( RtspClient ));
	memset( client, 0, sizeof( RtspClient ));
	strcpy( client->rtsp_url,  DEF_RTSP_URL );
	client->maxBufSize = 1024*1024;
	client->recv_buf = malloc(client->maxBufSize);
	if( !client->recv_buf )
	{
		printf("malloc error!\r\n");
		return;
	}
	rtsp_work_thread( (void *)client );




#if 0
	RTP_Init();
	if( argc == 2 )
	{
		receive_udp();
		return;
	}

	HISystem_Start();
	HANDLE *vdec = Hi264DecCreate();
	if( vdec == NULL )
	{
		printf("create vdec error!\r\n");
		return;
	}
	Hi264Init( vdec );
	RtspClient	*client = malloc( sizeof( RtspClient ));
	memset( client, 0, sizeof( RtspClient ));
	strcpy( client->rtsp_url,  DEF_RTSP_URL );
	client->maxBufSize = 1024*1024;
	client->recv_buf = malloc(client->maxBufSize);
	client->Vdec = vdec;
	if( !client->recv_buf )
	{
		printf("malloc error!\r\n");
		return;
	}
	rtsp_work_thread( (void *)client );

	FilterInit();
	HANDLE *nf = NewFilter( 1024*1024 , vdec );
	if( nf == NULL )
	{
		printf("create Filter error!\r\n");
		return;
	}
	RtspClient	*client = malloc( sizeof( RtspClient ));
	memset( client, 0, sizeof( RtspClient ));
	strcpy( client->rtsp_url,  DEF_RTSP_URL );
	client->maxBufSize = 1024*1024;
	client->recv_buf = malloc(client->maxBufSize);
	client->Filter = nf;
	client->Vdec = vdec;
	if( !client->recv_buf )
	{
		printf("malloc error!\r\n");
		return;
	}
	rtsp_work_thread( (void *)client );
#else
#if 0
	 H264DEC_Init( "192.168.1.92", 0, 0);
	 while( 1 )
		 usleep( 10000 );
#endif
#endif
}