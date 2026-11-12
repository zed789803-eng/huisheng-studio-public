#include "DriftBridge.h"
#include <cstdio>
#include <cmath>
#include <vector>
using namespace hs;
static void run(double ppmIn, uint32_t target, int seconds){
  DriftBridge b; b.reset(48000); b.setTarget(target);
  std::vector<float> in(4096*2), out(256*2);
  double frac=0; uint64_t produced=0, silent=0;
  const uint32_t cb=128; int n=(int)(48000.0*seconds/cb);
  double minB=1e9,maxB=0; int transitions=0; int lastSign=0;
  uint32_t prevUnder=0;
  for(int blk=0; blk<n; blk++){
    frac += cb*(1.0+ppmIn);
    uint32_t k=(uint32_t)frac; frac-=k;
    for(uint32_t i=0;i<k;i++){ float v=0.5f*std::sin(2*M_PI*997.0*(produced+i)/48000.0); in[i*2]=v; in[i*2+1]=v; }
    if(k) b.push(in.data(), k);
    produced+=k;
    b.pull(out.data(), cb);
    uint32_t bu=b.buffered(); if(blk>200){ minB=std::min(minB,(double)bu); maxB=std::max(maxB,(double)bu); }
    for(uint32_t i=0;i<cb;i++) if(out[i*2]==0.0f) silent++;
    int sgn = b.appliedRatioPPM()>5?-1:(b.appliedRatioPPM()<-5?1:0);
    if(sgn && lastSign && sgn!=lastSign) transitions++;
    if(sgn) lastSign=sgn;
    (void)prevUnder;
  }
  printf("%+6.1f ppm tgt=%3u %3ds: buffer %.0f..%.0f (target %u) under=%u over=%u ppm=%+.0f flips=%d silence=%.3fs\n",
    ppmIn*1e6,target,seconds,minB,maxB,target,b.underruns(),b.overruns(),b.appliedRatioPPM(),transitions,silent/48000.0);
}
int main(){
  run(5.5e-6,128,120); run(-5.5e-6,128,120);
  run(50e-6,128,120);  run(-50e-6,128,120);
  run(0,128,120);
  run(5.5e-6,256,120);
  return 0;
}
