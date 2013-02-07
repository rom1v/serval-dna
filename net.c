/*
Serval Distributed Numbering Architecture (DNA)
Copyright (C) 2012 Serval Project Inc.

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

#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>

#include "serval.h"
#include "conf.h"
#include "net.h"
#include "str.h"

struct in_addr hton_in_addr(in_addr_t addr)
{
  struct in_addr a;
  a.s_addr = htonl(addr);
  return a;
}

int _set_nonblock(int fd, struct __sourceloc __whence)
{
  int flags;
  if ((flags = fcntl(fd, F_GETFL, NULL)) == -1)
    return WHYF_perror("set_nonblock: fcntl(%d,F_GETFL,NULL)", fd);
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
    return WHYF_perror("set_nonblock: fcntl(%d,F_SETFL,0x%x|O_NONBLOCK)", fd, flags);
  return 0;
}

int _set_block(int fd, struct __sourceloc __whence)
{
  int flags;
  if ((flags = fcntl(fd, F_GETFL, NULL)) == -1)
    return WHYF_perror("set_block: fcntl(%d,F_GETFL,NULL)", fd);
  if (fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == -1)
    return WHYF_perror("set_block: fcntl(%d,F_SETFL,0x%x&~O_NONBLOCK)", fd, flags);
  return 0;
}

ssize_t _read_nonblock(int fd, void *buf, size_t len, struct __sourceloc __whence)
{
  ssize_t nread = read(fd, buf, len);
  if (nread == -1) {
    switch (errno) {
      case EINTR:
      case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
      case EWOULDBLOCK:
#endif
	return 0;
    }
    return WHYF_perror("read_nonblock: read(%d,%p,%lu)", fd, buf, (unsigned long)len);
  }
  return nread;
}

ssize_t _write_all(int fd, const void *buf, size_t len, struct __sourceloc __whence)
{
  ssize_t written = write(fd, buf, len);
  if (written == -1)
    return WHYF_perror("write_all: write(%d,%p %s,%lu)",
	fd, buf, alloca_toprint(30, buf, len), (unsigned long)len);
  if (written != len)
    return WHYF_perror("write_all: write(%d,%p %s,%lu) returned %ld",
	fd, buf, alloca_toprint(30, buf, len), (unsigned long)len, (long)written);
  return written;
}

ssize_t _write_nonblock(int fd, const void *buf, size_t len, struct __sourceloc __whence)
{
  ssize_t written = write(fd, buf, len);
  if (written == -1) {
    switch (errno) {
      case EINTR:
      case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
      case EWOULDBLOCK:
#endif
	return 0;
    }
    return WHYF_perror("write_nonblock: write(%d,%p %s,%lu)",
	fd, buf, alloca_toprint(30, buf, len), (unsigned long)len);
    return -1;
  }
  return written;
}

ssize_t _write_all_nonblock(int fd, const void *buf, size_t len, struct __sourceloc __whence)
{
  ssize_t written = _write_nonblock(fd, buf, len, __whence);
  if (written != -1 && written != len)
    return WHYF("write_all_nonblock: write(%d,%p %s,%lu) returned %ld",
	fd, buf, alloca_toprint(30, buf, len), (unsigned long)len, (long)written);
  return written;
}

ssize_t _write_str(int fd, const char *str, struct __sourceloc __whence)
{
  return _write_all(fd, str, strlen(str), __whence);
}

ssize_t _write_str_nonblock(int fd, const char *str, struct __sourceloc __whence)
{
  return _write_all_nonblock(fd, str, strlen(str), __whence);
}

ssize_t recvwithttl(int sock,unsigned char *buffer, size_t bufferlen,int *ttl,
		    struct sockaddr *recvaddr, socklen_t *recvaddrlen)
{
  struct msghdr msg;
  struct iovec iov[1];
  
  iov[0].iov_base=buffer;
  iov[0].iov_len=bufferlen;
  bzero(&msg,sizeof(msg));
  msg.msg_name = recvaddr;
  msg.msg_namelen = *recvaddrlen;
  msg.msg_iov = &iov[0];
  msg.msg_iovlen = 1;
  // setting the following makes the data end up in the wrong place
  //  msg.msg_iov->iov_base=iov_buffer;
  // msg.msg_iov->iov_len=sizeof(iov_buffer);
  
  struct cmsghdr cmsgcmsg[16];
  msg.msg_control = &cmsgcmsg[0];
  msg.msg_controllen = sizeof(struct cmsghdr)*16;
  msg.msg_flags = 0;
  
  ssize_t len = recvmsg(sock,&msg,0);
  if (len == -1 && errno != EAGAIN && errno != EWOULDBLOCK)
    return WHY_perror("recvmsg");
  
  if (0 && config.debug.packetrx) {
    DEBUGF("recvmsg returned %lld (flags=%d, msg_controllen=%d)", (long long) len, msg.msg_flags, msg.msg_controllen);
    dump("received data", buffer, len);
  }
  
  struct cmsghdr *cmsg;
  if (len>0)
  {
    for (cmsg = CMSG_FIRSTHDR(&msg); 
	 cmsg != NULL; 
	 cmsg = CMSG_NXTHDR(&msg,cmsg)) {
      
      if ((cmsg->cmsg_level == IPPROTO_IP) && 
	  ((cmsg->cmsg_type == IP_RECVTTL) ||(cmsg->cmsg_type == IP_TTL))
	  &&(cmsg->cmsg_len) ){
	if (config.debug.packetrx)
	  DEBUGF("  TTL (%p) data location resolves to %p", ttl,CMSG_DATA(cmsg));
	if (CMSG_DATA(cmsg)) {
	  *ttl = *(unsigned char *) CMSG_DATA(cmsg);
	  if (config.debug.packetrx)
	    DEBUGF("  TTL of packet is %d", *ttl);
	} 
      } else {
	if (config.debug.packetrx)
	  DEBUGF("I didn't expect to see level=%02x, type=%02x",
		 cmsg->cmsg_level,cmsg->cmsg_type);
      }	 
    }
  }
  *recvaddrlen=msg.msg_namelen;
  
  return len;
}
