#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>

// QUBO solver (heuristic - no proof)
// Solves random instances of QUBO problem as described in section 3.2 of
// http://www.cs.amherst.edu/ccm/cf14-mcgeoch.pdf
// 
// Chimera graph, C_N:
// Vertices are (x,y,o,i)  0<=x,y<N, 0<=o<2, 0<=i<4
// Edge from (x,y,o,i) to (y',x',o',i') if
// (x,y)=(x',y'), o!=o', OR
// |x-x'|=1, y=y', o=o'=0, i=i', OR
// |y-y'|=1, x=x', o=o'=1, i=i'
// 
// x,y are the horizontal,vertical co-ords of the K4,4
// o is the "orientation" (0=horizontally connected, 1=vertically connected)
// i is the index within the "semi-K4,4"

#define L 3
#define N 8
// Require 2^L >= N, but N doesn't have to be a power of 2

#define HORIZ 3
#define VERT (1<<(L+3))
#define M (1<<(2*L+3))
#define NV (8*N*N)
#define NE (8*N*(3*N-1))
#define NBV (2*N*N)
#define NBE (N*(3*N-2))
#define enc(x,y,o,i) ((i)+((o)<<2)+((x)<<3)+((y)<<(L+3)))
#define decx(p) (((p)>>3)&((1<<L)-1))
#define decy(p) (((p)>>(L+3))&((1<<L)-1))
#define deco(p) (((p)>>2)&1)
#define deci(p) ((p)&3)
// Bit encoding of vertex:
// 876543210
// \|/\|/|\|
//  |  | | Bits 0,1 = i
//  |  | Bit 2 = o
//  |  Bits 3..(L+2) = x
//  Bit (L+3)..(2L+2) = y

// Convention is that smaller numbers always correspond to horizontal

int adj[M][6];// Neighbours as encoded vertices (-1 for non-existent)
              // 0-3 corresponds to intra-K_4,4 neighbours
              // 4 = Left or Down, 5 = Right or Up
int Q[M][6];// Weights
int vlist[NV]; // encoded vertex list
int elist[NE][4]; // elist[e][0] = encoded start vertex of edge e
                  // elist[e][1] = edge number (0..5) from vertex elist[e][0] to elist[e][2]
                  // elist[e][2] = encoded end vertex of edge e
                  // elist[e][3] = edge number (0..5) from vertex elist[e][2] to elist[e][0]
int XBplus[N+2][N][2];
int (*XB)[N][2]=&XBplus[1];// XB[x][y][o] = State (0..15) of big vert
                           // Allow extra space to avoid having to check for out-of-bounds accesses
int QB[N][N][2][16][16][3]; // Weights for big verts (derived from Q[])
                            // QC[x][y][o][s0][s1][d] = total weight from big vert (x,y,o) in state s0
                            //                          to the big vert in direction d in state s1
                            // d=0 is intra K_4,4, d=1 is Left/Down, d=2 is Right/Up
#define X(p) ((XB[decx(p)][decy(p)][deco(p)]>>deci(p))&1)

#define MAX(x,y) ((x)>(y)?(x):(y))
void initrand(){srandom(time(0));}
int randbit(void){return (random()>>16)&1;}
int randnib(void){return (random()>>16)&15;}

double cpu(){return clock()/(double)CLOCKS_PER_SEC;}

void initgraph(void){
  int i,j,o,p,q,x,y,nv,ne;
  nv=ne=0;
  for(x=0;x<N;x++)for(y=0;y<N;y++)for(o=0;o<2;o++)for(i=0;i<4;i++){
    p=enc(x,y,o,i);vlist[nv++]=p;
    for(j=0;j<4;j++)adj[p][j]=enc(x,y,1-o,j);
    if(o==0){
      adj[p][4]=x>0?enc(x-1,y,o,i):-1;
      adj[p][5]=x<N-1?enc(x+1,y,o,i):-1;
    }else{
      adj[p][4]=y>0?enc(x,y-1,o,i):-1;
      adj[p][5]=y<N-1?enc(x,y+1,o,i):-1;
    }
    for(j=0;j<6;j++){
      q=adj[p][j];
      if(p<q){
        elist[ne][0]=p;elist[ne][1]=j;
        elist[ne][2]=q;elist[ne][3]=(j<4?i:9-j);
        ne++;
      }
    }
  }
  assert(nv==NV&&ne==NE);
}

void initweights(void){// Initialise a symmetric weight matrix with random +/-1s
  int i,j;
  for(i=0;i<NV;i++)for(j=0;j<6;j++)Q[i][j]=0; // Ensure weight=0 for non-existent edges
  for(i=0;i<NE;i++)
    Q[elist[i][0]][elist[i][1]]=Q[elist[i][2]][elist[i][3]]=randbit()*2-1;
}

void getbigweights(void){// Get derived weights on "big graph" QB[] from Q[]
  int i,j,o,p,t,x,y,s0,s1;
  for(x=0;x<N;x++)for(y=0;y<N;y++)for(o=0;o<2;o++)for(s0=0;s0<16;s0++)for(s1=0;s1<16;s1++){
    p=enc(x,y,o,0);
    t=0;
    for(i=0;i<4;i++)for(j=0;j<4;j++)t+=Q[p+i][j]*((s0>>i)&1)*((s1>>j)&1);
    QB[x][y][o][s0][s1][0]=t;
    t=0;
    for(i=0;i<4;i++)t+=Q[p+i][4]*((s0>>i)&1)*((s1>>i)&1);
    QB[x][y][o][s0][s1][1]=t;
    t=0;
    for(i=0;i<4;i++)t+=Q[p+i][5]*((s0>>i)&1)*((s1>>i)&1);
    QB[x][y][o][s0][s1][2]=t;
  }
}

void init_state(void){// Initialise state randomly
  int x,y,o;
  for(x=0;x<N;x++)for(y=0;y<N;y++)for(o=0;o<2;o++)XB[x][y][o]=randnib();
}

void writeweights(char *f){
  int i,v0,v1;
  FILE *fp;
  fp=fopen(f,"w");assert(fp);
  fprintf(fp,"%d %d\n",N,N);
  for(i=0;i<NE;i++){
    v0=elist[i][0];v1=elist[i][2];
    fprintf(fp,"%d %d %d %d   %d %d %d %d   %8d\n",
            decx(v0),decy(v0),deco(v0),deci(v0),
            decx(v1),decy(v1),deco(v1),deci(v1),
            Q[v0][elist[i][1]]);
  }
  fclose(fp);
}

void readweights(char *f){
  int i,j,w,x0,y0,o0,i0,e0,x1,y1,o1,i1,e1,nx,ny;
  FILE *fp;
  fp=fopen(f,"r");assert(fp);
  assert(fscanf(fp,"%d %d",&nx,&ny)==2);
  assert(nx==N&&ny==N);
  for(i=0;i<NV;i++)for(j=0;j<6;j++)Q[i][j]=0; // Ensure weight=0 for non-existent edges
  for(i=0;i<NE;i++){
    assert(fscanf(fp,"%d %d %d %d %d %d %d %d %d",
                  &x0,&y0,&o0,&i0,
                  &x1,&y1,&o1,&i1,
                  &w)==9);
    if(x1==x0&&y1==y0){assert(o0!=o1);e0=i1;e1=i0;}else{
      if(abs(x1-x0)==1&&y1==y0&&o0==0&&o1==0){e0=4+(x1-x0+1)/2;e1=9-e0;}else
        if(x1==x0&&abs(y1-y0)==1&&o0==1&&o1==1){e0=4+(y1-y0+1)/2;e1=9-e0;}else assert(0);
    }
    Q[enc(x0,y0,o0,i0)][e0]=Q[enc(x1,y1,o1,i1)][e1]=w;
  }
  fclose(fp);
}

int val(void){
  int v,x,y;
  v=0;
  for(x=0;x<N;x++)for(y=0;y<N;y++){
    v+=QB[x][y][0][XB[x][y][0]][XB[x][y][1]][0];
    v+=QB[x][y][0][XB[x][y][0]][XB[x+1][y][0]][2];
    v+=QB[x][y][1][XB[x][y][1]][XB[x][y+1][1]][2];
  }
  return v;
}

int dval(int p){
  int j,v,v0;
  v=0;
  for(j=0;j<6;j++){
    v0=adj[p][j];
    if(v0>=0)v+=Q[p][j]*X(v0);
  }
  return v;
}

int opt0(double maxt){// Simple K_4,4-wise optimisation
  int r,v,x,y,s0,s1,bv,cv,vmin;
  long long int nn;
  double t0,t1,tt;
  bv=1000000000;nn=0;t0=cpu();t1=0;
  while(cpu()-t0<maxt){
    init_state();
    cv=val();
    do{
      for(x=0;x<N;x++)for(y=0;y<N;y++){
        vmin=1000000000;
        for(s0=0;s0<16;s0++)for(s1=0;s1<16;s1++){
          v=QB[x][y][0][s0][s1][0];
          v+=QB[x][y][0][s0][XB[x-1][y][0]][1];
          v+=QB[x][y][0][s0][XB[x+1][y][0]][2];
          v+=QB[x][y][1][s1][XB[x][y-1][1]][1];
          v+=QB[x][y][1][s1][XB[x][y+1][1]][2];
          if(v<vmin){vmin=v;XB[x][y][0]=s0;XB[x][y][1]=s1;}
        }
      }
      v=val();r=(v<cv);if(r)cv=v;
    }while(r);
    nn++;
    tt=cpu()-t0;
    if(cv<bv||tt>=t1){
      if(cv<bv)bv=cv;
      printf("%12lld %10d %8.2f\n",nn,bv,tt);
      t1=MAX(tt*1.2,tt+5);
      fflush(stdout);
    }
  }
  return bv;
}

int lineexhaust(int c,int d){
  // If d=0 exhaust column c, else exhaust row c
  // Comments and variable names are as if column case (d=0)
  
  int b,o,r,s,v,smin0,smin1,vmin0,vmin1;
  int v0[16],v1[16];
  int h0[16][N][2],h1[16][N][2];// history

  if(d)return 0;//alter
  
  for(r=0;r<N;r++){
    for(b=0;b<16;b++){// b = state of (c,r,1)
      if(r>0){
        vmin0=1000000000;smin0=-1;
        for(s=0;s<16;s++){// s = state of (c,r-1,1)
          v=v0[s]+QB[c][r-1][1][s][b][2];
          if(v<vmin0){vmin0=v;smin0=s;}
        }
        memcpy(h1[b],h0[smin0],(2*r-1)*sizeof(int));
        h1[b][r-1][1]=smin0;
      }else vmin0=0;
      vmin1=1000000000;smin1=-1;
      for(s=0;s<16;s++){// s = state of (c,r,0)
        v=QB[c][r][0][s][b][0]+
          QB[c][r][0][s][XB[c-1][r][0]][1]+
          QB[c][r][0][s][XB[c+1][r][0]][2];
        if(v<vmin1){vmin1=v;smin1=s;}
      }
      v1[b]=vmin0+vmin1;
      h1[b][r][0]=smin1;
    }//b
    memcpy(v0,v1,sizeof(v0));
    memcpy(h0,h1,sizeof(h0));
  }//r

  vmin0=1000000000;smin0=-1;
  for(s=0;s<16;s++){// s = state of (c,N-1,1)
    v=v0[s];
    if(v<vmin0){vmin0=v;smin0=s;}
  }
  for(r=0;r<N;r++)for(o=0;o<2;o++)XB[c][r][o]=h0[smin0][r][o];
  XB[c][N-1][1]=smin0;
  return vmin0;
}

int opt1(double maxt){// Optimisation using line (column/row) exhausts
  int o,r,v,x,bv,cv;
  long long int nn;
  double t0,t1,tt;
  bv=1000000000;nn=0;t0=cpu();t1=0;
  while(cpu()-t0<maxt){
    init_state();
    cv=val();
    r=0;
    while(1){
      for(o=0;o<2;o++)for(x=0;x<N;x++){
        lineexhaust(x,o);
        v=val();
        if(v<cv){cv=v;r=0;}else{r+=1;if(r==2*N)goto el0;}
      }
    }
  el0:
    nn++;
    tt=cpu()-t0;
    if(cv<bv||tt>=t1){
      if(cv<bv)bv=cv;
      printf("%12lld %10d %8.2f\n",nn,bv,tt);
      t1=MAX(tt*1.2,tt+5);
      fflush(stdout);
    }
  }
  return bv;
}

int main(int ac,char**av){
  printf("N=%d\n",N);
  memset(XBplus,0,sizeof(XBplus));
  initrand();
  initgraph();
  if(ac<2){initweights();printf("Initialising random weight matrix\n");}else
    {readweights(av[1]);printf("Reading weight matrix from file \"%s\"\n",av[1]);}
  getbigweights();
  writeweights("tempproblem");
  opt1(1e100);
  return 0;
}
