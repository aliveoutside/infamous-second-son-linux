// SPDX-License-Identifier: GPL-2.0-or-later
// Native audio output with per-channel conversion and real-time buffer pacing.
#include "runtime.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
typedef struct {int active,channels,frames,floating,standard;int32_t volume[8];SDL_AudioStream *stream;uint64_t next;} Port;
typedef struct {int32_t handle;const void *data;} Output;
static HostLock lock=HOST_LOCK_INIT;
static Port ports[25];static int initialized;
static int32_t err(unsigned n){return (int32_t)(UINT32_C(0x80260000)|n);}
static int32_t GUEST_ABI init(void){
    if(initialized)return err(14);if(!SDL_InitSubSystem(SDL_INIT_AUDIO)){fprintf(stderr,"Audio init: %s\n",SDL_GetError());return err(15);}initialized=1;return 0;
}
static Port *port(int handle){int i=handle&255;return (handle&0x3f000000)==0x20000000&&i<25&&ports[i].active?&ports[i]:NULL;}
static int32_t GUEST_ABI open_audio(int user,int type,int index,uint32_t frames,uint32_t frequency,uint32_t param){
    (void)user;(void)index;if(!initialized)return err(15);if(!frames||frames>2048||(frames&255))return err(6);if(frequency!=48000)return err(8);
    unsigned format=param&255;if(format>7)return err(7);static const int channels[]={1,2,8,1,2,8,8,8};
    int first,last;switch(type){case 0:first=0;last=7;break;case 1:first=last=8;break;case 2:first=9;last=12;break;case 3:first=13;last=16;break;case 4:first=17;last=20;break;case 126:first=21;last=22;break;case 127:first=23;last=24;break;default:return err(10);}
    host_lock(&lock);int id;for(id=first;id<=last&&ports[id].active;++id){}if(id>last){host_unlock(&lock);return err(5);}
    Port *p=&ports[id];memset(p,0,sizeof(*p));p->channels=channels[format];p->frames=(int)frames;p->floating=format>=3&&format!=6;p->standard=format>=6;
    SDL_AudioSpec spec={SDL_AUDIO_F32,p->channels,48000};p->stream=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,NULL,NULL);
    if(!p->stream){fprintf(stderr,"Audio open: %s\n",SDL_GetError());host_unlock(&lock);return err(3);}
    for(int i=0;i<8;++i)p->volume[i]=32768;p->active=1;SDL_ResumeAudioStreamDevice(p->stream);host_unlock(&lock);
    printf("Audio opened: %d channels, %u frames, 48000 Hz\n",p->channels,frames);return 0x20000000|(type<<16)|id;
}
static int32_t output_one(int handle,const void *data,int pace){
    host_lock(&lock);Port *p=port(handle);if(!p){host_unlock(&lock);return err(1);}
    uint64_t period=(uint64_t)p->frames*1000000000/48000,now=SDL_GetTicksNS();
    if(!p->next||now>p->next+8*period)p->next=now;
    uint64_t deadline=p->next;p->next+=period;
    float converted[2048*8];static const int remap[]={0,1,2,3,6,7,4,5};
    if(data){for(int f=0;f<p->frames;++f)for(int c=0;c<p->channels;++c){int target=p->channels==8&&!p->standard?remap[c]:c;int from=f*p->channels+c,to=f*p->channels+target;
        float value=p->floating?((const float *)data)[from]:(float)((const int16_t *)data)[from]/32768.0f;
        converted[to]=value*(float)p->volume[c]/32768.0f;}
        if(!SDL_PutAudioStreamData(p->stream,converted,p->frames*p->channels*4)){host_unlock(&lock);return err(4);}
    }else SDL_ClearAudioStream(p->stream);
    host_unlock(&lock);if(pace){now=SDL_GetTicksNS();if(now<deadline)SDL_DelayPrecise(deadline-now);}return 0;
}
static int32_t GUEST_ABI output_audio(int handle,const void *data){return output_one(handle,data,1);}
static int32_t GUEST_ABI outputs_audio(const Output *list,uint32_t count){if(!list||!count||count>25)return err(4);for(uint32_t i=0;i<count;++i){int32_t e=output_one(list[i].handle,list[i].data,i+1==count);if(e)return e;}return 0;}
static int32_t GUEST_ABI close_audio(int handle){host_lock(&lock);Port *p=port(handle);if(!p){host_unlock(&lock);return err(1);}SDL_DestroyAudioStream(p->stream);p->active=0;host_unlock(&lock);return 0;}
static int32_t GUEST_ABI volume_audio(int handle,uint32_t mask,const int32_t *values){if(!values)return err(4);host_lock(&lock);Port *p=port(handle);if(!p){host_unlock(&lock);return err(1);}for(int i=0;i<p->channels;++i)if(mask&(1u<<i)){if(values[i]<0||values[i]>32768){host_unlock(&lock);return err(9);}p->volume[i]=values[i];}host_unlock(&lock);return 0;}
uintptr_t audio_resolve(const char *identity){
    if(!strstr(identity,"#libSceAudioOut#1#libSceAudioOut#257"))return 0;const char *api=runtime_api_name(identity);
#define API(n,f) if(!strcmp(api,n))return (uintptr_t)f
    API("sceAudioOutInit",init);API("sceAudioOutOpen",open_audio);API("sceAudioOutOutput",output_audio);API("sceAudioOutOutputs",outputs_audio);API("sceAudioOutClose",close_audio);API("sceAudioOutSetVolume",volume_audio);
#undef API
    return 0;
}
