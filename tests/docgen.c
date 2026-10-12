#include <stdio.h>
#include <stdlib.h>
#include "UIFoundation/NSDocFormatWriter.h"
int main(int argc,char**argv){ (void)argc;
    FILE*f=fopen(argv[1],"rb"); fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    char*b=malloc(n+1); fread(b,1,n,f); fclose(f); b[n]=0;
    unsigned char*out; size_t outLen;
    if(UIDocFormatWriterGenerate(b,n,&out,&outLen)!=0){fprintf(stderr,"gen fail\n");return 1;}
    FILE*g=fopen(argv[2],"wb"); fwrite(out,1,outLen,g); fclose(g);
    free(b); free(out); return 0;
}
