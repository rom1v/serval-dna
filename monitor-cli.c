/* 
Copyright (C) 2012 Paul Gardner-Stephen 

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

#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>

#include "serval.h"
#include "cli.h"
#include "monitor-client.h"

int remote_print(char *cmd, int argc, char **argv, unsigned char *data, int dataLen, void *context){
  int i;
  printf("%s",cmd);
  for (i=0;i<argc;i++){
    printf(" %s",argv[i]);
  }
  printf("\n");
  if (dataLen){
    dump(NULL,data,dataLen);
  }
  return 1;
}

struct monitor_command_handler monitor_handlers[]={
  {.command="",      .handler=remote_print},
};

int app_monitor_cli(int argc, const char *const *argv, struct command_line_option *o, void *context)
{
  struct pollfd fds[2];
  struct monitor_state *state;
  
  int monitor_client_fd = monitor_client_open(&state);
  
  set_nonblock(STDIN_FILENO);
  set_nonblock(monitor_client_fd);
  
  fds[0].fd = STDIN_FILENO;
  fds[0].events = POLLIN;
  fds[1].fd = monitor_client_fd;
  fds[1].events = POLLIN;
  
  while(1){
    int r = poll(fds, 2, 100);
    if (r>0){
      
      if (fds[0].revents & POLLIN){
	char buff[256];
	int bytes = read(STDIN_FILENO, buff, sizeof(buff));
	set_block(monitor_client_fd);
	write(monitor_client_fd, buff, bytes);
	set_nonblock(monitor_client_fd);
      }
      
      if (fds[1].revents & POLLIN){
	if (monitor_client_read(monitor_client_fd, state, monitor_handlers, 
				sizeof(monitor_handlers)/sizeof(struct monitor_command_handler))<0){
	  break;
	}
      }
      
      if (fds[0].revents & (POLLHUP | POLLERR))
	break;
    }
  }
  
  monitor_client_close(monitor_client_fd, state);
  monitor_client_fd=-1;
  
  return 0;
}

