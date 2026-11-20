#include <stdio.h>
#include <stdlib.h>
#include "../main/core/ingot.h"
static void dump(const char*fn, uint8_t*buf, metal_params_t*mp){
  uint16_t lut[256]; lut_metal(lut, mp);
  FILE*f=fopen(fn,"wb"); fprintf(f,"P6 %d %d 255\n",SCR_W,SCR_H);
  for(int i=0;i<SCR_W*SCR_H;i++){uint16_t c=buf[i]?lut[buf[i]]:0; unsigned char p[3]={(c>>11)<<3,((c>>5)&63)<<2,(c&31)<<3}; fwrite(p,1,3,f);} fclose(f);}
int main(){
  uint8_t*base=malloc(SCR_W*SCR_H),*front=malloc(SCR_W*SCR_H),*back=malloc(SCR_W*SCR_H);
  mint_rec_t m={0}; m.minted=1; m.p.price=91.42; m.p.buy_ts=1795269060; m.p.shares=1; m.serial=0x4f2a91c3;
  ingot_gen_base(base,123,0); ingot_layout_t lay; ingot_gen_front(front,base,&m,&lay);
  ingot_gen_base(back,321,1);
  metal_params_t p={1,0,0,1,0};
  dump("/tmp/f_new.ppm",front,&p); p.patina=patina_for_days(365); dump("/tmp/f_1y.ppm",front,&p);
  p.patina=patina_for_days(365*5); dump("/tmp/f_5y.ppm",front,&p);
  p.patina=0; p.finish=0; dump("/tmp/raw.ppm",base,&p); p.heat=0.6; dump("/tmp/hot.ppm",base,&p);
  p.finish=1;p.heat=0; dump("/tmp/back.ppm",back,&p);
}
