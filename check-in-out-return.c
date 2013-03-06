#include <stdio.h>
#include <string.h>

int line_count=0;
char *filename=NULL;

int dofile(char *file)
{
  FILE *f=fopen(file,"r");
  char line[1024];
  if (filename) free(filename);
  filename=strdup(file);
  line_count=0;

  int depth=0;
  int inoutActive=0;
  int savedActive=0;
  int commentMode=0;
  int quoteMode=0;

  line[0]=0; fgets(line,1024,f);
  while(line[0]) {
    line_count++;
    int i;
    for(i=0;i<strlen(line);i++) {
      if (!strncmp(&line[i],"//",2)) break;
      if (!strncmp(&line[i],"/*",2)) commentMode=1;
      if (!strncmp(&line[i],"*/",2)) commentMode=0;
      if (!strncmp(&line[i],"\"",1)) quoteMode^=1;
      if (commentMode||quoteMode) continue;
      if (line[i]=='{') depth++;
      if (line[i]=='}') {
	if (depth>0) depth--;
	if (!depth) {
	  if (inoutActive) 
	    fprintf(stderr,"%s:%d:Leaving function without OUT() or RETURN?\n",
		    filename,line_count);
	  inoutActive=0;
	  savedActive=0;
	} else inoutActive=savedActive;
      }
      if (!strncmp(&line[i],"IN()",4)) { inoutActive=1; savedActive=1; }
      if (!strncmp(&line[i],"OUT()",5)) inoutActive=0;
      if (inoutActive&&
	  (!strncmp(&line[i],"return",6)))
	{
	  fprintf(stderr,
		  "%s:%d:Called return instead of RETURN in function with IN()\n",
		  filename,line_count);
	}
    }

    line[0]=0; fgets(line,1024,f);
  }
}

int main(int argc,char **argv)
{
  int i;
  for(i=1;i<argc;i++) dofile(argv[i]);
}
