/* lat.c — end-to-end latency probe: replay test request bodies against a running
 * api over one keep-alive TCP connection (concurrency 1) and report percentiles.
 * Isolates per-request server latency from queueing/contention.
 * Usage: lat <host> <port> <test-data.json> [nreq]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }
static int cmpd(const void*a,const void*b){ double x=*(const double*)a,y=*(const double*)b; return (x>y)-(x<y); }

int main(int argc,char**argv){
    if(argc<4){fprintf(stderr,"usage: %s <host> <port> <test.json> [nreq]\n",argv[0]);return 2;}
    int port=atoi(argv[2]); int nreq=argc>4?atoi(argv[4]):20000;

    /* slurp test-data, collect request-object bodies {...} after each "request": */
    FILE*f=fopen(argv[3],"rb"); if(!f){perror("open");return 1;}
    fseek(f,0,SEEK_END); long fl=ftell(f); rewind(f);
    char*buf=malloc(fl+1); if(fread(buf,1,fl,f)!=(size_t)fl){perror("read");return 1;} buf[fl]=0; fclose(f);

    int cap=60000,nb=0; char**body=malloc(cap*sizeof(char*)); int*blen=malloc(cap*sizeof(int));
    const char*cur=buf;
    for(;;){ const char*rq=strstr(cur,"\"request\""); if(!rq)break;
        const char*br=strchr(rq,'{'); if(!br)break;
        int depth=0; const char*e=br;
        for(;*e;e++){ if(*e=='{')depth++; else if(*e=='}'){depth--; if(depth==0){e++;break;}} }
        int len=(int)(e-br); body[nb]=malloc(len+1); memcpy(body[nb],br,len); body[nb][len]=0; blen[nb]=len;
        nb++; cur=e; if(nb>=cap)break;
    }
    fprintf(stderr,"replay bodies=%d, sending %d requests\n",nb,nreq);

    int s=socket(AF_INET,SOCK_STREAM,0);
    struct sockaddr_in sa={0}; sa.sin_family=AF_INET; sa.sin_port=htons(port);
    inet_pton(AF_INET,argv[1],&sa.sin_addr);
    if(connect(s,(struct sockaddr*)&sa,sizeof(sa))<0){perror("connect");return 1;}
    int one=1; setsockopt(s,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));

    char req[4096], rbuf[8192];
    double*lat=malloc((size_t)nreq*sizeof(double));
    double t0=now();
    for(int i=0;i<nreq;i++){
        const char*b=body[i%nb]; int bl=blen[i%nb];
        int rl=snprintf(req,sizeof(req),
            "POST /fraud-score HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s",bl,b);
        double q0=now();
        if(write(s,req,rl)!=rl){perror("write");return 1;}
        /* read until we have a full response (headers + body); responses are tiny */
        int got=0; for(;;){ ssize_t n=read(s,rbuf+got,sizeof(rbuf)-got); if(n<=0){perror("read");return 1;} got+=n;
            /* one response ends after a JSON body; detect end via trailing '}' or content-length close enough — assume tiny single response per write */
            if(memmem(rbuf,got,"\r\n\r\n",4)){ /* have headers; assume body fits this read */ break; } }
        lat[i]=(now()-q0)*1000.0; /* ms */
    }
    double dt=now()-t0;
    qsort(lat,nreq,sizeof(double),cmpd);
    double sum=0; for(int i=0;i<nreq;i++)sum+=lat[i];
    printf("requests=%d  wall=%.3fs  throughput=%.0f req/s (concurrency=1)\n",nreq,dt,nreq/dt);
    printf("latency ms: avg=%.4f  p50=%.4f  p90=%.4f  p99=%.4f  p99.9=%.4f  max=%.4f\n",
        sum/nreq, lat[nreq/2], lat[(int)(nreq*0.90)], lat[(int)(nreq*0.99)], lat[(int)(nreq*0.999)], lat[nreq-1]);
    close(s); return 0;
}
