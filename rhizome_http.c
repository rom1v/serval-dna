/*
Serval Distributed Numbering Architecture (DNA)
Copyright (C) 2010 Paul Gardner-Stephen
 
This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.
 
This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
 
You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <sys/types.h>
#include <sys/socket.h>
#include <signal.h>

#include "serval.h"
#include "str.h"
#include "rhizome.h"

typedef struct rhizome_http_request {
  struct sched_ent alarm;
  long long initiate_time; /* time connection was initiated */
  
  /* The HTTP request as currently received */
  int request_length;
#define RHIZOME_HTTP_REQUEST_MAXLEN 1024
  char request[RHIZOME_HTTP_REQUEST_MAXLEN];
  
  /* Nature of the request */
  int request_type;
#define RHIZOME_HTTP_REQUEST_RECEIVING -1
#define RHIZOME_HTTP_REQUEST_FROMBUFFER 1
#define RHIZOME_HTTP_REQUEST_FILE 2
#define RHIZOME_HTTP_REQUEST_SUBSCRIBEDGROUPLIST 4
#define RHIZOME_HTTP_REQUEST_ALLGROUPLIST 8
#define RHIZOME_HTTP_REQUEST_BUNDLESINGROUP 16
  // manifests are small enough to send from a buffer
  // #define RHIZOME_HTTP_REQUEST_BUNDLEMANIFEST 32
  // for anything too big, we can just use a blob
#define RHIZOME_HTTP_REQUEST_BLOB 64
#define RHIZOME_HTTP_REQUEST_FAVICON 128
  
  /* Local buffer of data to be sent.
   If a RHIZOME_HTTP_REQUEST_FROMBUFFER, then the buffer is sent, and when empty
   the request is closed.
   Else emptying the buffer triggers a request to fetch more data.  Only if no
   more data is provided do we then close the request. */
  unsigned char *buffer;
  int buffer_size; // size
  int buffer_length; // number of bytes loaded into buffer
  int buffer_offset; // where we are between [0,buffer_length)
  
  /* The source specification data which are used in different ways by different 
   request types */
  char source[1024];
  long long source_index;
  long long source_count;
  int source_record_size;
  unsigned int source_flags;
  
  sqlite3_blob *blob;
  /* source_index used for offset in blob */
  long long blob_end; 
  
} rhizome_http_request;

static int rhizome_server_free_http_request(rhizome_http_request *r);
static int rhizome_server_http_send_bytes(rhizome_http_request *r);
static int rhizome_server_parse_http_request(rhizome_http_request *r);
static int rhizome_server_simple_http_response(rhizome_http_request *r, int result, const char *response);
static int rhizome_server_http_response_header(rhizome_http_request *r, int result, const char *mime_type, unsigned long long bytes);
static int rhizome_server_sql_query_fill_buffer(rhizome_http_request *r, char *table, char *column);

#define RHIZOME_SERVER_MAX_LIVE_REQUESTS 32

struct sched_ent server_alarm;
struct profile_total server_stats;

struct profile_total connection_stats;

/*
  HTTP server and client code for rhizome transfers.
 */

unsigned short rhizome_http_server_port = 0;
static int rhizome_server_socket = -1;
static time_ms_t rhizome_server_last_start_attempt = -1;

// Format icon data using:
//   od -vt u1 ~/Downloads/favicon.ico | cut -c9- | sed 's/  */,/g'
unsigned char favicon_bytes[]={
0,0,1,0,1,0,16,16,16,0,0,0,0,0,40,1
,0,0,22,0,0,0,40,0,0,0,16,0,0,0,32,0
,0,0,1,0,4,0,0,0,0,0,128,0,0,0,0,0
,0,0,0,0,0,0,16,0,0,0,0,0,0,0,104,158
,168,0,163,233,247,0,104,161,118,0,0,0,0,0,0,0
,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
,0,0,0,0,0,0,0,0,0,0,0,0,0,0,17,17
,17,17,17,18,34,17,17,18,34,17,17,18,34,17,17,2
,34,17,17,18,34,17,16,18,34,1,17,17,1,17,1,17
,1,16,1,16,17,17,17,17,1,17,16,16,17,17,17,17
,1,17,18,34,17,17,17,16,17,17,2,34,17,17,17,16
,17,16,18,34,17,17,17,16,17,1,17,1,17,17,17,18
,34,17,17,16,17,17,17,18,34,17,17,18,34,17,17,18
,34,17,17,18,34,17,17,16,17,17,17,18,34,17,17,16
,17,17,17,17,17,0,17,1,17,17,17,17,17,17,0,0
,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
int favicon_len=318;

int rhizome_http_server_running()
{
  return rhizome_server_socket != -1;
}

/* Start the Rhizome HTTP server by creating a socket, binding it to an available port, and
   marking it as passive.  If called repeatedly and frequently, this function will only try to start
   the server after a certain time has elapsed since the last attempt.
   Return -1 if an error occurs (message logged).
   Return 0 if the server was started.
   Return 1 if the server is already started successfully.
   Return 2 if the server was not started because it is too soon since last failed attempt.
 */
int rhizome_http_server_start()
{
  if (rhizome_server_socket != -1)
    return 1;

  /* Only try to start http server every five seconds. */
  time_ms_t now = gettime_ms();
  if (now < rhizome_server_last_start_attempt + 5000)
    return 2;
  rhizome_server_last_start_attempt  = now;
  if (debug & DEBUG_RHIZOME_TX)
    DEBUGF("Starting rhizome HTTP server");

  unsigned short port;
  for (port = RHIZOME_HTTP_PORT; port <= RHIZOME_HTTP_PORT_MAX; ++port) {
    /* Create a new socket, reusable and non-blocking. */
    if (rhizome_server_socket == -1) {
      rhizome_server_socket = socket(AF_INET,SOCK_STREAM,0);
      if (rhizome_server_socket == -1) {
	WHY_perror("socket");
	goto error;
      }
      int on=1;
      if (setsockopt(rhizome_server_socket, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on)) == -1) {
	WHY_perror("setsockopt(REUSEADDR)");
	goto error;
      }
      if (ioctl(rhizome_server_socket, FIONBIO, (char *)&on) == -1) {
	WHY_perror("ioctl(FIONBIO)");
	goto error;
      }
    }
    /* Bind it to the next port we want to try. */
    struct sockaddr_in address;
    bzero((char *) &address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    if (bind(rhizome_server_socket, (struct sockaddr *) &address, sizeof(address)) == -1) {
      if (errno != EADDRINUSE) {
	WHY_perror("bind");
	goto error;
      }
    } else {
      /* We bound to a port.  The battle is half won.  Now we have to successfully listen on that
	port, which could also fail with EADDRINUSE, in which case we have to scrap the socket and
	create a new one, because once bound, a socket stays bound.
      */
      if (listen(rhizome_server_socket, 20) != -1)
	goto success;
      if (errno != EADDRINUSE) {
	WHY_perror("listen");
	goto error;
      }
      close(rhizome_server_socket);
      rhizome_server_socket = -1;
    }
  }
  WHYF("No ports available in range %u to %u", RHIZOME_HTTP_PORT, RHIZOME_HTTP_PORT_MAX);
error:
  if (rhizome_server_socket != -1) {
    close(rhizome_server_socket);
    rhizome_server_socket = -1;
  }
  return WHY("Failed to start rhizome HTTP server");

success:
  INFOF("RHIZOME HTTP SERVER, START port=%d, fd=%d", port, rhizome_server_socket);
  rhizome_http_server_port = port;
  /* Add Rhizome HTTPd server to list of file descriptors to watch */
  server_alarm.function = rhizome_server_poll;
  server_stats.name="rhizome_server_poll";
  server_alarm.stats=&server_stats;
  server_alarm.poll.fd = rhizome_server_socket;
  server_alarm.poll.events = POLLIN;
  watch(&server_alarm);
  return 0;

}

void rhizome_client_poll(struct sched_ent *alarm)
{
  rhizome_http_request *r = (rhizome_http_request *)alarm;
  if (alarm->poll.revents == 0){
    rhizome_server_free_http_request(r);
    return;
  }
  switch(r->request_type)
    {
    case RHIZOME_HTTP_REQUEST_RECEIVING:
      /* Keep reading until we have two CR/LFs in a row */
      r->request[r->request_length] = '\0';
      sigPipeFlag=0;
      int bytes = read_nonblock(r->alarm.poll.fd, &r->request[r->request_length], RHIZOME_HTTP_REQUEST_MAXLEN - r->request_length);
      /* If we got some data, see if we have found the end of the HTTP request */
      if (bytes > 0) {
	// reset inactivity timer
	r->alarm.alarm = gettime_ms() + RHIZOME_IDLE_TIMEOUT;
	r->alarm.deadline = r->alarm.alarm + RHIZOME_IDLE_TIMEOUT;
	unschedule(&r->alarm);
	schedule(&r->alarm);
	r->request_length += bytes;
	if (http_header_complete(r->request, r->request_length, bytes + 4)) {
	  /* We have the request. Now parse it to see if we can respond to it */
	  rhizome_server_parse_http_request(r);
	}
      } else {
	if (debug & DEBUG_RHIZOME_TX)
	  DEBUG("Empty read, closing connection");
	rhizome_server_free_http_request(r);
	return;
      }
      if (sigPipeFlag) {
	if (debug & DEBUG_RHIZOME_TX)
	  DEBUG("Received SIGPIPE, closing connection");
	rhizome_server_free_http_request(r);
	return;
      }
      break;
    default:
      /* Socket already has request -- so just try to send some data. */
      rhizome_server_http_send_bytes(r);
      break;
  }
  return;
}

void rhizome_server_poll(struct sched_ent *alarm)
{
  if (alarm->poll.revents & (POLLIN | POLLOUT)) {
    struct sockaddr addr;
    unsigned int addr_len = sizeof addr;
    int sock;
    while ((sock = accept(rhizome_server_socket, &addr, &addr_len)) != -1) {
      if (addr.sa_family == AF_INET) {
	struct sockaddr_in *peerip = (struct sockaddr_in *)&addr;
	INFOF("RHIZOME HTTP SERVER, ACCEPT addrlen=%u family=%u port=%u addr=%u.%u.%u.%u",
	    addr_len, peerip->sin_family, peerip->sin_port,
	    ((unsigned char*)&peerip->sin_addr.s_addr)[0],
	    ((unsigned char*)&peerip->sin_addr.s_addr)[1],
	    ((unsigned char*)&peerip->sin_addr.s_addr)[2],
	    ((unsigned char*)&peerip->sin_addr.s_addr)[3]
	  );
      } else {
	INFOF("RHIZOME HTTP SERVER, ACCEPT addrlen=%u family=%u data=%s",
	  addr_len, addr.sa_family, alloca_tohex((unsigned char *)addr.sa_data, sizeof addr.sa_data)
	);
      }
      rhizome_http_request *request = calloc(sizeof(rhizome_http_request), 1);
      if (request == NULL) {
	WHYF_perror("calloc(%u, 1)", sizeof(rhizome_http_request));
	WHY("Cannot respond to request, out of memory");
      } else {
	/* We are now trying to read the HTTP request */
	request->request_type=RHIZOME_HTTP_REQUEST_RECEIVING;
	request->alarm.function = rhizome_client_poll;
	connection_stats.name="rhizome_client_poll";
	request->alarm.stats=&connection_stats;
	request->alarm.poll.fd=sock;
	request->alarm.poll.events=POLLIN;
	request->alarm.alarm = gettime_ms()+RHIZOME_IDLE_TIMEOUT;
	request->alarm.deadline = request->alarm.alarm+RHIZOME_IDLE_TIMEOUT;
	// watch for the incoming http request
	watch(&request->alarm);
	// set an inactivity timeout to close the connection
	schedule(&request->alarm);
      }
    }
    if (errno != EAGAIN) {
      WARN_perror("accept");
    }
  }
  
  if (alarm->poll.revents & (POLLHUP | POLLERR)) {
    INFO("Error on tcp listen socket");
  }
}

static int rhizome_server_free_http_request(rhizome_http_request *r)
{
  unwatch(&r->alarm);
  unschedule(&r->alarm);
  close(r->alarm.poll.fd);
  if (r->buffer)
    free(r->buffer);
  if (r->blob)
    sqlite3_blob_close(r->blob);
  free(r);
  return 0;
}

static int rhizome_server_sql_query_http_response(rhizome_http_request *r,
					   char *column,char *table,char *query_body,
					   int bytes_per_row,int dehexP)
{
  /* Run the provided SQL query progressively and return the values of the first
     column it returns.  As the result list may be very long, we will add the
     LIMIT <skip>,<count> clause to do it piece by piece.

     Otherwise, the response is prefixed by a 256 byte header, including the public
     key of the sending node, and allowing space for information about encryption of
     the body, although encryption is not yet implemented here.
 */

  if (r->buffer == NULL || r->buffer_size < 16384) {
    if (r->buffer)
      free(r->buffer);
    r->buffer_size = 16384;
    r->buffer = malloc(r->buffer_size);
    if (r->buffer == NULL) {
      r->buffer_size = 0;
      WHY_perror("malloc");
      return WHY("Cannot send response, out of memory");
    }
  }
  r->buffer_length=0;
  r->buffer_offset=0;
  r->source_record_size=bytes_per_row;
  r->source_count = 0;
  sqlite_exec_int64(&r->source_count, "SELECT COUNT(*) %s", query_body);

  /* Work out total response length */
  long long response_bytes=256+r->source_count*r->source_record_size;
  rhizome_server_http_response_header(r, 200, "servalproject.org/rhizome-list", response_bytes);
  if (debug & DEBUG_RHIZOME_TX)
    DEBUGF("headers consumed %d bytes", r->buffer_length);

  /* Clear and prepare response header */
  bzero(&r->buffer[r->buffer_length],256);
  
  r->buffer[r->buffer_length]=0x01; /* type of response (list) */
  r->buffer[r->buffer_length+1]=0x01; /* version of response */

  if (debug & DEBUG_RHIZOME_TX)
    DEBUGF("Found %lld records",r->source_count);
  /* Number of records we intend to return */
  r->buffer[r->buffer_length+4]=(r->source_count>>0)&0xff;
  r->buffer[r->buffer_length+5]=(r->source_count>>8)&0xff;
  r->buffer[r->buffer_length+6]=(r->source_count>>16)&0xff;
  r->buffer[r->buffer_length+7]=(r->source_count>>24)&0xff;

  r->buffer_length+=256;

  /* copy our public key in to bytes 32+ */
  // TODO get out public key (SID) from keyring and copy into response packet

  /* build templated query */
  strbuf b = strbuf_local(r->source, sizeof r->source);
  strbuf_sprintf(b, "SELECT %s,rowid %s", column, query_body);
  if (strbuf_overrun(b))
    WHYF("SQL query overrun: %s", strbuf_str(b));
  r->source_index=0;
  r->source_flags=dehexP;

  DEBUGF("buffer_length=%d",r->buffer_length);

  /* Populate spare space in buffer with rows of data */
  return rhizome_server_sql_query_fill_buffer(r, table, column);
}

static int rhizome_server_sql_query_fill_buffer(rhizome_http_request *r, char *table, char *column)
{
  unsigned char blob_value[r->source_record_size*2+1];

  if (debug & DEBUG_RHIZOME_TX)
    DEBUGF("populating with sql rows at offset %d",r->buffer_length);
  if (r->source_index>=r->source_count)
    {
      /* All done */
      return 0;
    }

  int record_count=(r->buffer_size-r->buffer_length)/r->source_record_size;
  if (record_count<1) {
    if (debug & DEBUG_RHIZOME_TX)
      DEBUGF("r->buffer_size=%d, r->buffer_length=%d, r->source_record_size=%d",
	   r->buffer_size, r->buffer_length, r->source_record_size);
    return WHY("Not enough space to fit any records");
  }

  sqlite3_stmt *statement = sqlite_prepare("%s LIMIT %lld,%d", r->source, r->source_index, record_count);
  if (!statement)
    return -1;
  if (debug & DEBUG_RHIZOME_TX)
    DEBUG(sqlite3_sql(statement));
  sqlite_retry_state retry = SQLITE_RETRY_STATE_DEFAULT;
  while(  r->buffer_length + r->source_record_size < r->buffer_size
      &&  sqlite_step_retry(&retry, statement) == SQLITE_ROW
  ) {
    r->source_index++;
    if (sqlite3_column_count(statement)!=2) {
      sqlite3_finalize(statement);
      return WHY("sqlite3 returned multiple columns for a single column query");
    }
    sqlite3_blob *blob;
    const unsigned char *value;
    int column_type=sqlite3_column_type(statement, 0);
    switch(column_type) {
    case SQLITE_TEXT:	value=sqlite3_column_text(statement, 0); break;
    case SQLITE_BLOB:
      if (debug & DEBUG_RHIZOME_TX)
	DEBUGF("table='%s',col='%s',rowid=%lld", table, column, sqlite3_column_int64(statement,1));

      int ret;
      int64_t rowid = sqlite3_column_int64(statement, 1);
      do ret = sqlite3_blob_open(rhizome_db, "main", table, column, rowid, 0 /* read only */, &blob);
	while (sqlite_code_busy(ret) && sqlite_retry(&retry, "sqlite3_blob_open"));
      if (!sqlite_code_ok(ret)) {
	WHYF("sqlite3_blob_open() failed, %s", sqlite3_errmsg(rhizome_db));
	continue;
      }
      sqlite_retry_done(&retry, "sqlite3_blob_open");
      if (sqlite3_blob_read(blob,&blob_value[0],
			/* copy number of bytes based on whether we need to
			    de-hex the string or not */
			    r->source_record_size*(1+(r->source_flags&1)),0)
	  !=SQLITE_OK) {
	WHYF("sqlite3_blob_read() failed, %s", sqlite3_errmsg(rhizome_db));
	sqlite3_blob_close(blob);
	continue;
      }
      value = blob_value;
      sqlite3_blob_close(blob);
      break;
    default:
      /* improper column type, so don't include in report */
      WHYF("Bad column type %d", column_type);
      continue;
    }
    if (r->source_flags&1) {
      /* hex string to be converted */
      int i;
      for(i=0;i<r->source_record_size;i++)
	/* convert the two nybls and make a byte */
	r->buffer[r->buffer_length+i]
	  =(hexvalue(value[i<<1])<<4)|hexvalue(value[(i<<1)+1]);
    } else
      /* direct binary value */
      bcopy(value,&r->buffer[r->buffer_length],r->source_record_size);
    r->buffer_length+=r->source_record_size;
    
  }
  sqlite3_finalize(statement);
  return 0;
}

int http_header_complete(const char *buf, size_t len, size_t tail)
{
  const char *bufend = buf + len;
  if (tail < len)
    buf = bufend - tail;
  int count = 0;
  for (; count < 2 && buf != bufend; ++buf) {
    switch (*buf) {
      case '\n': ++count; break;
      case '\r': break;
      case '\0': break; // ignore NUL (telnet inserts them)
      default: count = 0; break;
    }
  }
  return count == 2;
}

static int rhizome_server_parse_http_request(rhizome_http_request *r)
{
  /* Switching to writing, so update the call-back */
  r->alarm.poll.events=POLLOUT;
  watch(&r->alarm);
  // Start building up a response.
  r->request_type = 0;
  // Parse the HTTP "GET" line.
  char *path = NULL;
  size_t pathlen = 0;
  if (str_startswith(r->request, "GET ", &path)) {
    char *p;
    // This loop is guaranteed to terminate before the end of the buffer, because we know that the
    // buffer contains at least "\n\n" and maybe "\r\n\r\n" at the end of the header block.
    for (p = path; !isspace(*p); ++p)
      ;
    pathlen = p - path;
    if ( str_startswith(p, " HTTP/1.", &p)
      && (str_startswith(p, "0", &p) || str_startswith(p, "1", &p))
      && (str_startswith(p, "\r\n", &p) || str_startswith(p, "\n", &p))
    )
      path[pathlen] = '\0';
    else
      path = NULL;
  }
  if (path) {
    char *id = NULL;
    INFOF("RHIZOME HTTP SERVER, GET %s", alloca_toprint(1024, path, pathlen));
    if (strcmp(path, "/favicon.ico") == 0) {
      r->request_type = RHIZOME_HTTP_REQUEST_FAVICON;
      rhizome_server_http_response_header(r, 200, "image/vnd.microsoft.icon", favicon_len);
    } else if (strcmp(path, "/rhizome/groups") == 0) {
      /* Return the list of known groups */
      rhizome_server_sql_query_http_response(r, "id", "groups", "from groups", 32, 1);
    } else if (strcmp(path, "/rhizome/files") == 0) {
      /* Return the list of known files */
      rhizome_server_sql_query_http_response(r, "id", "files", "from files", 32, 1);
    } else if (strcmp(path, "/rhizome/bars") == 0) {
      /* Return the list of known BARs */
      rhizome_server_sql_query_http_response(r, "bar", "manifests", "from manifests", 32, 0);
    } else if (str_startswith(path, "/rhizome/file/", &id)) {
      /* Stream the specified payload */
      if (!rhizome_str_is_file_hash(id)) {
	rhizome_server_simple_http_response(r, 400, "<html><h1>Invalid payload ID</h1></html>\r\n");
      } else {
	// TODO: Check for Range: header and return 206 if returning partial content
	str_toupper_inplace(id);
	long long rowid = -1;
	sqlite_exec_int64(&rowid, "select rowid from files where id='%s';", id);
	if (rowid >= 0 && sqlite3_blob_open(rhizome_db, "main", "files", "data", rowid, 0, &r->blob) != SQLITE_OK)
	  rowid = -1;
	if (rowid == -1) {
	  rhizome_server_simple_http_response(r, 404, "<html><h1>Payload not found</h1></html>\r\n");
	} else {
	  r->source_index = 0;
	  r->blob_end = sqlite3_blob_bytes(r->blob);
	  rhizome_server_http_response_header(r, 200, "application/binary", r->blob_end - r->source_index);
	  r->request_type |= RHIZOME_HTTP_REQUEST_BLOB;
	}
      }
    } else if (str_startswith(path, "/rhizome/manifest/", &id)) {
      // TODO: Stream the specified manifest
      rhizome_server_simple_http_response(r, 500, "<html><h1>Not implemented</h1></html>\r\n");
    } else {
      rhizome_server_simple_http_response(r, 404, "<html><h1>Not found</h1></html>\r\n");
    }
  } else {
    if (debug & DEBUG_RHIZOME_TX)
      DEBUGF("Received malformed HTTP request: %s", alloca_toprint(120, (const char *)r->request, r->request_length));
    rhizome_server_simple_http_response(r, 400, "<html><h1>Malformed request</h1></html>\r\n");
  }
  
  /* Try sending data immediately. */
  rhizome_server_http_send_bytes(r);

  return 0;
}


/* Return appropriate message for HTTP response codes, both known and unknown. */
static const char *httpResultString(int response_code) {
  switch (response_code) {
  case 200: return "OK";
  case 206: return "Partial Content";
  case 404: return "Not found";
  case 500: return "Internal server error";
  default:  return "A suffusion of yellow";
  }
}

struct http_response {
  unsigned int result_code;
  const char * content_type;
  unsigned long long content_length;
  const char * body;
};

static strbuf strbuf_build_http_response(strbuf sb, const struct http_response *h)
{
  strbuf_sprintf(sb, "HTTP/1.0 %03u %s\r\n", h->result_code, httpResultString(h->result_code));
  strbuf_sprintf(sb, "Content-type: %s\r\n", h->content_type);
  strbuf_sprintf(sb, "Content-length: %llu\r\n", h->content_length);
  strbuf_puts(sb, "\r\n");
  if (h->body)
    strbuf_puts(sb, h->body);
  return sb;
}

static int rhizome_server_set_response(rhizome_http_request *r, const struct http_response *h)
{
  strbuf b = strbuf_local((char *) r->buffer, r->buffer_size);
  strbuf_build_http_response(b, h);
  if (r->buffer == NULL || strbuf_overrun(b)) {
    // Need a bigger buffer
    if (r->buffer)
      free(r->buffer);
    r->buffer_size = strbuf_count(b) + 1;
    r->buffer = malloc(r->buffer_size);
    if (r->buffer == NULL) {
      WHYF_perror("malloc(%u)", r->buffer_size);
      r->buffer_size = 0;
      return WHY("Cannot send response, out of memory");
    }
    strbuf_init(b, (char *) r->buffer, r->buffer_size);
    strbuf_build_http_response(b, h);
    if (strbuf_overrun(b))
      return WHYF("Bug! Cannot send response, buffer not big enough");
  }
  r->buffer_length = strbuf_len(b);
  r->buffer_offset = 0;
  r->request_type |= RHIZOME_HTTP_REQUEST_FROMBUFFER;
  if (debug & DEBUG_RHIZOME_TX)
    DEBUGF("Sending HTTP response: %s", alloca_toprint(120, (const char *)r->buffer, r->buffer_length));
  return 0;
}

static int rhizome_server_simple_http_response(rhizome_http_request *r, int result, const char *response)
{
  struct http_response hr;
  hr.result_code = result;
  hr.content_type = "text/html";
  hr.content_length = strlen(response);
  hr.body = response;
  return rhizome_server_set_response(r, &hr);
}

static int rhizome_server_http_response_header(rhizome_http_request *r, int result, const char *mime_type, unsigned long long bytes)
{
  struct http_response hr;
  hr.result_code = result;
  hr.content_type = mime_type;
  hr.content_length = bytes;
  hr.body = NULL;
  return rhizome_server_set_response(r, &hr);
}

/*
  return codes:
  1: connection still open.
  0: connection finished.
  <0: an error occurred.
*/
static int rhizome_server_http_send_bytes(rhizome_http_request *r)
{
  // keep writing until the write would block or we run out of data
  while(r->request_type){
    
    /* Flush anything out of the buffer if present, before doing any further
       processing */
    if (r->request_type&RHIZOME_HTTP_REQUEST_FROMBUFFER)
      {
	int bytes=r->buffer_length-r->buffer_offset;
	bytes=write(r->alarm.poll.fd,&r->buffer[r->buffer_offset],bytes);
	if (bytes<=0){
	  // stop writing when the tcp buffer is full
	  // TODO errors?
	  return 1;
	}
	
	if (0)
	  dump("bytes written",&r->buffer[r->buffer_offset],bytes);
	r->buffer_offset+=bytes;
	  
	// reset inactivity timer
	r->alarm.alarm = gettime_ms()+RHIZOME_IDLE_TIMEOUT;
	r->alarm.deadline = r->alarm.alarm+RHIZOME_IDLE_TIMEOUT;
	unschedule(&r->alarm);
	schedule(&r->alarm);
	
	if (r->buffer_offset>=r->buffer_length) {
	  /* Buffer's cleared */
	  r->request_type&=~RHIZOME_HTTP_REQUEST_FROMBUFFER;
	  r->buffer_offset=0; r->buffer_length=0;
	}
	
	// go around the loop again to work out what we should do next
	continue;
	
      }

    switch(r->request_type&(~RHIZOME_HTTP_REQUEST_FROMBUFFER))
      {
      case RHIZOME_HTTP_REQUEST_FAVICON:
	if (r->buffer_size<favicon_len) {
	  free(r->buffer);
	  r->buffer_size=0;
	  r->buffer=malloc(favicon_len);
	  if (!r->buffer) r->request_type=0;
	}
	if (r->buffer)
	{
	    int i;
	    for(i=0;i<favicon_len;i++)
	      r->buffer[i]=favicon_bytes[i];
	    r->buffer_length=i;
	    if (debug & DEBUG_RHIZOME_TX)
	      DEBUGF("favicon buffer_length=%d\n", r->buffer_length);
	    r->request_type=RHIZOME_HTTP_REQUEST_FROMBUFFER;
	}
	break;
      case RHIZOME_HTTP_REQUEST_BLOB:
	{
	  /* Get more data from the file and put it in the buffer */
	  int read_size = 65536;
	  if (r->blob_end-r->source_index < read_size)
	    read_size = r->blob_end-r->source_index;
	    
	  r->request_type=0;
	  if (read_size>0){
	    
	    if (r->buffer_size < read_size) {
	      if (r->buffer)
		free(r->buffer);
	      r->buffer=malloc(read_size);
	      if (!r->buffer) {
		WHY_perror("malloc");
		r->request_type=0; break;
	      }
	      r->buffer_size=read_size;
	    }
	      
	    if(sqlite3_blob_read(r->blob,&r->buffer[0],read_size,r->source_index)
	       ==SQLITE_OK)
	      {
		r->buffer_length = read_size;
		r->source_index+=read_size;
		r->request_type|=RHIZOME_HTTP_REQUEST_FROMBUFFER;
	      }
	  }
	    
	  if (r->source_index >= r->blob_end){
	    sqlite3_blob_close(r->blob);
	    r->blob=0;
	  }else
	    r->request_type|=RHIZOME_HTTP_REQUEST_BLOB;
	}
	break;
	  
      default:
	WHY("sending data from this type of HTTP request not implemented");
	r->request_type=0;
	break;
      }
  }
  if (!r->request_type) return rhizome_server_free_http_request(r);	  
  return 1;
}
