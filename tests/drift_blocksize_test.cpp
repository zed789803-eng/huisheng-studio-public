#include "DriftBridge.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace hs;
// producer at rate inRate, consumer block `blk` at outRate, 5.5 ppm drift.
static void run(uint32_t inRate, uint32_t outRate, uint32_t blk, double ppm, int seconds){
  DriftBridge b; b.reset(outRate); b.setInputRate(inRate); b.setTarget(128);
  std::vector<float> in(8192*2), out(8192*2);
  double frac=0; uint64_t produced=0; int calls=(int)((double)outRate*seconds/blk);
  double minB=1e9,maxB=0; uint64_t silence=0; double peak=0;
  for(int c=0;c<calls;c++){
    frac += (double)blk*(double)inRate/outRate*(1.0+ppm);
    uint32_t n=(uint32_t)frac; frac-=n;
    for(uint32_t i=0;i<n;i++){ float v=0.5f*std::sin(2*M_PI*1000.0*(produced+i)/inRate); in[i*2]=v; in[i*2+1]=v; }
    if(n) b.push(in.data(),n);
    produced+=n;
    for(uint32_t i=0;i<blk*2;i++) out[i]=1234.0f;
    b.pull(out.data(), blk);
    // count frames the bridge itself zeroed as silence (excluding first prime)
    if(c>50){ minB=std::min(minB,(double)b.buffered()); maxB=std::max(maxB,(double)b.buffered()); }
    for(uint32_t i=0;i<blk;i++) if(out[i*2]==0.0f && c>50) silence++;
    for(uint32_t i=0;i<blk;i++) peak=std::max(peak,(double)std::fabs(out[i*2]));
  }
  printf("in=%u out=%u blk=%u ppm=%+.1f %ds: buffer %.0f..%.0f  zeroed=%llu  peak=%.3f  ppm_cmd=%+.0f\n",
    inRate,outRate,blk,ppm,seconds,minB,maxB,(unsigned long long)silence,peak,b.appliedRatioPPM());
}
int main(){
  run(48000,48000,128,5.5e-6,60);
  run(48000,48000,256,-5.5e-6,60);
  run(48000,48000,512,5.5e-6,60);
  run(44100,48000,128,0,30);
  run(48000,44100,256,0,30);
  run(96000,48000,64,0,30);
  return 0;
}
