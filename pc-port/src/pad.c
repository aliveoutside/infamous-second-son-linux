// SPDX-License-Identifier: GPL-2.0-or-later
// XInput plus a focused-window keyboard controller, including the two-contact gate, and the mouse
// as touchpad (F6), spray can (F7) or aim and fire (F4), and the touchpad window (F5, src/pad_panel.c).
#include "runtime.h"
#include "pad_panel.h"
#include <SDL3/SDL.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <wchar.h>
#include <SDL3/SDL.h>
#include "pad_panel.h"
int save_dialog_blocking(void); // src/save_dialog.c
typedef struct {uint16_t x,y;uint8_t id,reserved[3];} Touch;
typedef struct {
    uint32_t buttons;uint8_t lx,ly,rx,ry,l2,r2,pad[2];
    float orientation[4],acceleration[3],angular[3];
    uint8_t touch_count,touch_pad[3];uint32_t held;Touch touch[2];
    uint8_t connected,pad2[3];uint64_t timestamp;uint8_t extension[16];
    uint8_t connected_count,reserved[2],unique_length,unique[12];
} PadData;
typedef struct {float density;uint16_t width,height;uint8_t left_dead,right_dead,type,count,connected,pad[3];int32_t device_class;uint8_t reserved[8];} PadInfo;
_Static_assert(sizeof(PadData)==120&&sizeof(PadInfo)==28,"Pad ABI");
static HostLock lock=HOST_LOCK_INIT;
static int initialized,opened,was_touch[2];
static uint8_t next_touch=1,contact_id[2];
static uint64_t touch_start;
static SDL_Gamepad *sdl_pad;
static uint64_t scan_tick,sensor_tick;
static float quaternion[4]={0,0,0,1};
static int sdl_ready;
static int swipe_direction=-1,swipe_keys[4];
static uint64_t swipe_started;
static void scan_gamepads(void){
    uint64_t now=SDL_GetTicks();
    if(sdl_pad&&!SDL_GamepadConnected(sdl_pad)){fprintf(stderr,"PAD disconnected\n");SDL_CloseGamepad(sdl_pad);sdl_pad=NULL;scan_tick=0;}
    if(scan_tick&&now-scan_tick<1000)return;scan_tick=now;
    int count=0;SDL_JoystickID *ids=SDL_GetGamepads(&count);SDL_JoystickID chosen=0;int score=-1;
    for(int i=0;i<count;++i){SDL_GamepadType type=SDL_GetGamepadTypeForID(ids[i]);int s=(type==SDL_GAMEPAD_TYPE_PS4||type==SDL_GAMEPAD_TYPE_PS5)?2:1;
        if(s>score){score=s;chosen=ids[i];}}
    SDL_free(ids);
    if(!chosen||(sdl_pad&&SDL_GetGamepadID(sdl_pad)==chosen))return;
    SDL_Gamepad *next=SDL_OpenGamepad(chosen);if(!next){fprintf(stderr,"PAD open failed: %s\n",SDL_GetError());return;}
    if(sdl_pad)SDL_CloseGamepad(sdl_pad);sdl_pad=next;
    memset(was_touch,0,sizeof(was_touch));memset(quaternion,0,sizeof(quaternion));quaternion[3]=1;sensor_tick=0;
    bool gyro=SDL_SetGamepadSensorEnabled(next,SDL_SENSOR_GYRO,true);
    bool accel=SDL_SetGamepadSensorEnabled(next,SDL_SENSOR_ACCEL,true);
    fprintf(stderr,"PAD selected name=%s joystick=%s type=%d vendor=%04x product=%04x touchpads=%d gyro=%d accel=%d\n",
        SDL_GetGamepadName(next),SDL_GetJoystickName(SDL_GetGamepadJoystick(next)),(int)SDL_GetGamepadType(next),SDL_GetGamepadVendor(next),SDL_GetGamepadProduct(next),SDL_GetNumGamepadTouchpads(next),gyro,accel);
}
static uint16_t touch_coord(float v,unsigned maximum){if(!isfinite(v)||v<0)v=0;if(v>1)v=1;return (uint16_t)(v*(float)maximum);}
// The controller layout (`pad_map` in settings.toml, ISS_PAD_MAP="a=cross,view=touch_left,..."): what
// each button of a controller without a touchpad does. PlayStation controllers keep their own
// layout (Options, the real touchpad; Share/Create does nothing, as on the console). The names
// are source_names and the targets below.
enum {SRC_A,SRC_B,SRC_X,SRC_Y,SRC_LB,SRC_RB,SRC_LT,SRC_RT,SRC_LS,SRC_RS,SRC_VIEW,SRC_MENU,SRC_GUIDE,SRC_SHARE,
      SRC_UP,SRC_DOWN,SRC_LEFT,SRC_RIGHT,SRC_COUNT};
static const char *const source_names[SRC_COUNT]={"a","b","x","y","lb","rb","lt","rt","ls","rs","view","menu","guide","share",
    "up","down","left","right"};
enum {T_NONE,T_CROSS,T_CIRCLE,T_SQUARE,T_TRIANGLE,T_L1,T_R1,T_L2,T_R2,T_L3,T_R3,T_OPTIONS,T_PS,T_UP,T_DOWN,T_LEFT,T_RIGHT,
      T_TOUCH,T_TOUCH_LEFT,T_TOUCH_RIGHT,T_SWIPE_UP,T_SWIPE_DOWN,T_SWIPE_LEFT,T_SWIPE_RIGHT,T_COUNT};
// L2/R2 carry the trigger's analog value; their button bits follow it (pad_read, above 30).
static const struct {const char *name;uint32_t bit;} targets[T_COUNT]={{"none",0},{"cross",0x4000},{"circle",0x2000},
    {"square",0x8000},{"triangle",0x1000},{"l1",0x400},{"r1",0x800},{"l2",0},{"r2",0},{"l3",2},{"r3",4},{"options",8},
    {"ps",0x10000},{"up",0x10},{"down",0x40},{"left",0x80},{"right",0x20},{"touch",0x100000},{"touch_left",0x100000},
    {"touch_right",0x100000},{"swipe_up",0},{"swipe_down",0},{"swipe_left",0},{"swipe_right",0}};
// Recommended (the user's layout, 2026-10-19): the PS4 layout one to one, and View / Menu press
// the left / right side of the touchpad, under the thumbs for the game's quick touchpad prompts.
// Guide is Options (pause): Menu is taken by the touchpad. The panel (F5) and Esc press it too.
static uint8_t pad_map[SRC_COUNT]={T_CROSS,T_CIRCLE,T_SQUARE,T_TRIANGLE,T_L1,T_R1,T_L2,T_R2,T_L3,T_R3,T_TOUCH_LEFT,
    T_TOUCH_RIGHT,T_OPTIONS,T_NONE,T_UP,T_DOWN,T_LEFT,T_RIGHT};
static const uint8_t playstation_map[SRC_COUNT]={T_CROSS,T_CIRCLE,T_SQUARE,T_TRIANGLE,T_L1,T_R1,T_L2,T_R2,T_L3,T_R3,T_NONE,
    T_OPTIONS,T_PS,T_NONE,T_UP,T_DOWN,T_LEFT,T_RIGHT};
// The keyboard's touchpad shortcuts (ISS_TOUCH_KEYS="touch_left=Space,swipe_up=1,..." with SDL key
// names). Tab stays a second key for the left side.
enum {K_TOUCH_LEFT,K_TOUCH_RIGHT,K_TOUCH_BOTH,K_TOUCH_CENTER,K_SWIPE_UP,K_SWIPE_DOWN,K_SWIPE_LEFT,K_SWIPE_RIGHT,K_MOUSE,K_SPRAY,K_PANEL,K_AIM,K_COUNT};
static const char *const touch_key_names[K_COUNT]={"touch_left","touch_right","touch_both","touch_center","swipe_up",
    "swipe_down","swipe_left","swipe_right","mouse_touchpad","spray","panel","aim"};
static SDL_Scancode touch_keys[K_COUNT]={SDL_SCANCODE_Z,SDL_SCANCODE_X,SDL_SCANCODE_F8,SDL_SCANCODE_T,SDL_SCANCODE_1,
    SDL_SCANCODE_2,SDL_SCANCODE_3,SDL_SCANCODE_4,SDL_SCANCODE_F6,SDL_SCANCODE_F7,SDL_SCANCODE_F5,SDL_SCANCODE_F4};
static int pad_swipe=-1,source_was[SRC_COUNT];
// Each stick's dead zone (`deadzone_left`, `deadzone_right` in settings.toml, ISS_PAD_DEADZONE_LEFT/
// _RIGHT: percent of full deflection, default 10): radial, and the rest of the range is stretched so
// the output still starts at zero.
static float stick_dead_left=0.10f,stick_dead_right=0.10f;
static void stick(int ax,int ay,float stick_dead,uint8_t *ox,uint8_t *oy){
    float x=ax/32767.f,y=ay/32767.f;const float m=sqrtf(x*x+y*y);
    if(m<=stick_dead){*ox=*oy=128;return;}
    // Each axis is clamped on its own afterwards: a corner still reaches full deflection on both.
    const float k=(m-stick_dead)/(1-stick_dead)/m;x*=k;y*=k;
    *ox=(uint8_t)lrintf(fminf(fmaxf(x*127.5f+127.5f,0),255));*oy=(uint8_t)lrintf(fminf(fmaxf(y*127.5f+127.5f,0),255));
}
static void load_controls(void){
    static const char *const dead_names[2]={"ISS_PAD_DEADZONE_LEFT","ISS_PAD_DEADZONE_RIGHT"};
    float *const dead_values[2]={&stick_dead_left,&stick_dead_right};
    for(int i=0;i<2;++i){const char *dead=getenv(dead_names[i]);
        if(dead&&*dead){const float d=(float)atof(dead)/100;*dead_values[i]=d>=0&&d<=0.5f?d:0.10f;}}
    const char *s=getenv("ISS_PAD_MAP");
    while(s&&*s){char item[64];size_t n=strcspn(s,",");if(n>=sizeof(item))n=sizeof(item)-1;memcpy(item,s,n);item[n]=0;s+=n;if(*s==',')++s;
        char *v=strchr(item,'=');if(!v)continue;*v++=0;
        for(int i=0;i<SRC_COUNT;++i)if(!strcmp(item,source_names[i]))for(int t=0;t<T_COUNT;++t)if(!strcmp(v,targets[t].name))pad_map[i]=(uint8_t)t;}
    s=getenv("ISS_TOUCH_KEYS");
    while(s&&*s){char item[64];size_t n=strcspn(s,",");if(n>=sizeof(item))n=sizeof(item)-1;memcpy(item,s,n);item[n]=0;s+=n;if(*s==',')++s;
        char *v=strchr(item,'=');if(!v)continue;*v++=0;
        for(int k=0;k<K_COUNT;++k)if(!strcmp(item,touch_key_names[k])){const SDL_Scancode code=SDL_GetScancodeFromName(v);
            if(code!=SDL_SCANCODE_UNKNOWN)touch_keys[k]=code;else fprintf(stderr,"PAD_MAP unknown key '%s' for %s\n",v,item);}}
    if(getenv("ISS_PAD_MAP")||getenv("ISS_TOUCH_KEYS")){fprintf(stderr,"PAD_MAP");for(int i=0;i<SRC_COUNT;++i)fprintf(stderr," %s=%s",source_names[i],targets[pad_map[i]].name);
        fprintf(stderr," keys");for(int k=0;k<K_COUNT;++k)fprintf(stderr," %s=%s",touch_key_names[k],SDL_GetScancodeName(touch_keys[k]));fputc('\n',stderr);}
}
static void apply_map(const uint8_t map[SRC_COUNT],const int pressed[SRC_COUNT],uint8_t lt,uint8_t rt,PadData *out,int contacts[2],uint16_t xy[2][2]){
    for(int i=0;i<SRC_COUNT;++i){const int t=map[i],down=pressed[i];
        if(down&&!source_was[i]&&t>=T_SWIPE_UP)pad_swipe=t-T_SWIPE_UP;
        source_was[i]=down;
        const uint8_t amount=i==SRC_LT?lt:i==SRC_RT?rt:(down?255:0);
        if(t==T_L2&&amount>out->l2)out->l2=amount;
        if(t==T_R2&&amount>out->r2)out->r2=amount;
        if(!down)continue;
        out->buttons|=targets[t].bit;
        if(t==T_TOUCH){contacts[0]=1;xy[0][0]=960;xy[0][1]=471;}
        if(t==T_TOUCH_LEFT){contacts[0]=1;xy[0][0]=480;xy[0][1]=471;}
        if(t==T_TOUCH_RIGHT){contacts[1]=1;xy[1][0]=1440;xy[1][1]=471;}
    }
}
// A controller without a touchpad: hold L3 + R3 (both stick clicks) and it becomes one. The right stick is the
// finger (its deflection is the position on the pad, centre in the middle), the D-pad swipes, A
// clicks. Those inputs are taken from the game while the layer is held.
static int touch_layer_was[4];
static void touch_layer(int pressed[SRC_COUNT],PadData *out,int contacts[2],uint16_t xy[2][2]){
    static const int dirs[4]={SRC_UP,SRC_DOWN,SRC_LEFT,SRC_RIGHT}; // swipe 0 up 1 down 2 left 3 right
    if(!(pressed[SRC_LS]&&pressed[SRC_RS])){memset(touch_layer_was,0,sizeof(touch_layer_was));return;}
    for(int i=0;i<4;++i){if(pressed[dirs[i]]&&!touch_layer_was[i])pad_swipe=i;touch_layer_was[i]=pressed[dirs[i]];pressed[dirs[i]]=0;}
    pressed[SRC_LS]=pressed[SRC_RS]=0;
    if(pressed[SRC_A]){out->buttons|=0x100000;pressed[SRC_A]=0;}
    contacts[0]=1;xy[0][0]=touch_coord(out->rx/255.f,1919);xy[0][1]=touch_coord(out->ry/255.f,941);
    out->rx=out->ry=128;
}
static PadLive live;
static int read_sdl(PadData *out,int contacts[2],uint16_t xy[2][2]){
    if(!sdl_ready)return 0;SDL_UpdateGamepads();scan_gamepads();if(!sdl_pad)return 0;
    static const SDL_GamepadButton buttons[SRC_COUNT]={SDL_GAMEPAD_BUTTON_SOUTH,SDL_GAMEPAD_BUTTON_EAST,SDL_GAMEPAD_BUTTON_WEST,
        SDL_GAMEPAD_BUTTON_NORTH,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,SDL_GAMEPAD_BUTTON_INVALID,
        SDL_GAMEPAD_BUTTON_INVALID,SDL_GAMEPAD_BUTTON_LEFT_STICK,SDL_GAMEPAD_BUTTON_RIGHT_STICK,SDL_GAMEPAD_BUTTON_BACK,
        SDL_GAMEPAD_BUTTON_START,SDL_GAMEPAD_BUTTON_GUIDE,SDL_GAMEPAD_BUTTON_MISC1,SDL_GAMEPAD_BUTTON_DPAD_UP,
        SDL_GAMEPAD_BUTTON_DPAD_DOWN,SDL_GAMEPAD_BUTTON_DPAD_LEFT,SDL_GAMEPAD_BUTTON_DPAD_RIGHT};
    stick(SDL_GetGamepadAxis(sdl_pad,SDL_GAMEPAD_AXIS_LEFTX),SDL_GetGamepadAxis(sdl_pad,SDL_GAMEPAD_AXIS_LEFTY),stick_dead_left,&out->lx,&out->ly);
    stick(SDL_GetGamepadAxis(sdl_pad,SDL_GAMEPAD_AXIS_RIGHTX),SDL_GetGamepadAxis(sdl_pad,SDL_GAMEPAD_AXIS_RIGHTY),stick_dead_right,&out->rx,&out->ry);
    int l=SDL_GetGamepadAxis(sdl_pad,SDL_GAMEPAD_AXIS_LEFT_TRIGGER),r=SDL_GetGamepadAxis(sdl_pad,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    const uint8_t lt=(uint8_t)(l>0?l*255/32767:0),rt=(uint8_t)(r>0?r*255/32767:0);
    int pressed[SRC_COUNT];
    for(int i=0;i<SRC_COUNT;++i)pressed[i]=i==SRC_LT?lt>30:i==SRC_RT?rt>30:SDL_GetGamepadButton(sdl_pad,buttons[i]);
    live.connected=1;live.sources=0;for(int i=0;i<SRC_COUNT;++i)if(pressed[i])live.sources|=UINT32_C(1)<<i;
    live.lx=out->lx;live.ly=out->ly;live.rx=out->rx;live.ry=out->ry;live.lt=lt;live.rt=rt;
    const SDL_GamepadType type=SDL_GetGamepadType(sdl_pad);
    if(!(type==SDL_GAMEPAD_TYPE_PS4||type==SDL_GAMEPAD_TYPE_PS5))touch_layer(pressed,out,contacts,xy);
    apply_map(type==SDL_GAMEPAD_TYPE_PS4||type==SDL_GAMEPAD_TYPE_PS5?playstation_map:pad_map,pressed,lt,rt,out,contacts,xy);
    if(SDL_GetGamepadButton(sdl_pad,SDL_GAMEPAD_BUTTON_TOUCHPAD))out->buttons|=0x100000;
    if(SDL_GetNumGamepadTouchpads(sdl_pad)>0){int fingers=SDL_GetNumGamepadTouchpadFingers(sdl_pad,0);
        for(int i=0;i<2&&i<fingers;++i){bool down=false;float x=0,y=0,pressure=0;
            if(SDL_GetGamepadTouchpadFinger(sdl_pad,0,i,&down,&x,&y,&pressure)&&down){contacts[i]=1;xy[i][0]=touch_coord(x,1919);xy[i][1]=touch_coord(y,941);}}}
    float gyro[3]={0},accel[3]={0,SDL_STANDARD_GRAVITY,0};
    SDL_GetGamepadSensorData(sdl_pad,SDL_SENSOR_GYRO,gyro,3);SDL_GetGamepadSensorData(sdl_pad,SDL_SENSOR_ACCEL,accel,3);
    for(int i=0;i<3;++i){out->angular[i]=isfinite(gyro[i])?gyro[i]:0;out->acceleration[i]=isfinite(accel[i])?accel[i]/SDL_STANDARD_GRAVITY:0;}
    uint64_t now=SDL_GetTicksNS();float dt=sensor_tick?(float)((double)(now-sensor_tick)/1e9):0;sensor_tick=now;
    // Same q * angular-velocity convention as shadPS4 input/controller.cpp (GPL-2.0-or-later).
    if(dt>0&&dt<0.1f){float x=quaternion[0],y=quaternion[1],z=quaternion[2],w=quaternion[3];
        float gx=out->angular[0]*dt*.5f,gy=out->angular[1]*dt*.5f,gz=out->angular[2]*dt*.5f;
        quaternion[0]+=w*gx+y*gz-z*gy;quaternion[1]+=w*gy+z*gx-x*gz;
        quaternion[2]+=w*gz+x*gy-y*gx;quaternion[3]-=x*gx+y*gy+z*gz;
        float norm=sqrtf(quaternion[0]*quaternion[0]+quaternion[1]*quaternion[1]+quaternion[2]*quaternion[2]+quaternion[3]*quaternion[3]);
        for(int i=0;i<4;++i)quaternion[i]/=norm;}
    memcpy(out->orientation,quaternion,sizeof(quaternion));return 1;
}
static int key(SDL_Scancode code){const bool *state=SDL_GetKeyboardState(NULL);return code>SDL_SCANCODE_UNKNOWN&&code<SDL_SCANCODE_COUNT&&state[code];}
static int shift_held(void){return key(SDL_SCANCODE_LSHIFT)||key(SDL_SCANCODE_RSHIFT);}
static int mouse_button(SDL_MouseButtonFlags mask){return (SDL_GetMouseState(NULL,NULL)&mask)!=0;}
// A minimal desktop PhonePad, in the focused game window. F6: the mouse is the touchpad (left
// button: a finger at the cursor, right button: touchpad click). F7: spray can (the graffiti
// missions): the controller is held on its side like a can, R2 on top and the light bar to the
// screen; the mouse turns it, the left button sprays (R2), the right button shakes it and the
// middle button recenters it, Shift aims finely. F4: the mouse is the right stick (look and aim),
// the left button fires (R2), the right button aims (L2), the middle button is R3 (mouse_sensitivity). The window title names the mode. The keys can be
// changed with ISS_TOUCH_KEYS (above).
static int mouse_mode,mode_keys[4],mouse_anchored,mouse_touch,spray_wait_release;
static int cmd_spray_toggle,cmd_spray_center;static double cmd_shake_until,cmd_aim_until,cmd_paint_until;static float cmd_aim_dx,cmd_aim_dy; // ISS_PAD_CMD_FILE spray (paint: R2 held)
static SDL_Window *game_window;
static float mouse_anchor_x,mouse_anchor_y;
static float spray[4];
static uint64_t spray_tick;
static const float spray_pose[4]={0,0,.70710678f,.70710678f}; // +90 degrees about Z: body +X (R side) up
static void quat_mul(const float a[4],const float b[4],float out[4]){
    out[0]=a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1];out[1]=a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0];
    out[2]=a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3];out[3]=a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2];
}
static void quat_rotate(const float q[4],const float v[3],float out[3]){ // q v q*
    const float t[3]={2*(q[1]*v[2]-q[2]*v[1]),2*(q[2]*v[0]-q[0]*v[2]),2*(q[0]*v[1]-q[1]*v[0])};
    out[0]=v[0]+q[3]*t[0]+q[1]*t[2]-q[2]*t[1];out[1]=v[1]+q[3]*t[1]+q[2]*t[0]-q[0]*t[2];out[2]=v[2]+q[3]*t[2]+q[0]*t[1]-q[1]*t[0];
}
// One read of the spray can: the mouse moved (dx, dy) pixels in dt seconds. The can turns about
// the world's vertical (right: aims right) and horizontal axes (down: aims down), ISS_SPRAY_SENS
// times 0.003 rad per pixel (PhonePad's Alt+mouse); the gyro gets the rate in the controller's
// own axes and the accelerometer gravity (in G), plus a 6 Hz shake along the can while asked.
void pad_spray_step(float q[4],float dx,float dy,float dt,int shake,double seconds,float angular[3],float accel[3]){
    static float sens=-1;if(sens<0){const char *s=getenv("ISS_SPRAY_SENS");sens=s?(float)atof(s):1;if(!(sens>0))sens=1;}
    const float world[3]={-.003f*sens*dy,-.003f*sens*dx,0};const float angle=sqrtf(world[0]*world[0]+world[1]*world[1]);
    if(angle>1e-7f){const float s=sinf(angle*.5f)/angle,r[4]={world[0]*s,world[1]*s,0,cosf(angle*.5f)};float n[4];quat_mul(r,q,n);
        const float norm=sqrtf(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]+n[3]*n[3]);for(int i=0;i<4;++i)q[i]=n[i]/norm;}
    const float conj[4]={-q[0],-q[1],-q[2],q[3]},up[3]={0,1,0},rate[3]={dt>0?world[0]/dt:0,dt>0?world[1]/dt:0,0};
    quat_rotate(conj,rate,angular);quat_rotate(conj,up,accel);
    if(shake){const float phase=(float)(6.283185307*6.0*seconds);accel[0]+=1.6f*sinf(phase);angular[2]+=4.0f*cosf(phase);}
}
// F4's aim. With native/mouse_look.cpp the camera takes the mouse's counts as an angle (pad_look_take,
// gathered here between the camera's updates; dropped when it has not asked for two reads, as in menus).
// Without it (another eboot) the mouse drives the right stick, and the stick sets the camera's turning
// speed: the distance is kept as a debt in pixels and paid out as the stick that turns the camera by
// that much in the time since the last payment (K pixels per second is full deflection). A flick faster
// than the stick can follow is paid out over the next frames (at most 0.25 s of debt is kept). K is
// 2400 px/s at 100% (ISS_MOUSE_SENS, percent; more percent, more turn per pixel). The game is told the
// right stick has no dead zone (pad_info), so a paid-out stick starts at 3%. The game may read the pad
// more than once a frame: a payment is made once 4 ms of mouse counts have gathered, and the stick is
// held between payments.
int pad_look_direct;
static float look[2];static int look_unread;
int pad_look_take(float *dx,float *dy){
    host_lock(&lock);*dx=look[0];*dy=look[1];look[0]=look[1]=0;look_unread=0;host_unlock(&lock);
    return mouse_mode==3;
}
static float aim_debt[2],aim_time;static uint8_t aim_out[2]={128,128};
static void aim_step(float dx,float dy,float dt){
    static float k=-1;if(k<0){const char *s=getenv("ISS_MOUSE_SENS");const float p=s?(float)atof(s)/100:1;k=2400.f/(p>0?p:1);}
    aim_debt[0]+=dx;aim_debt[1]+=dy;aim_time+=dt;
    if(aim_time<.004f)return;
    for(int i=0;i<2;++i){
        float s=aim_debt[i]/(k*aim_time);s=fminf(fmaxf(s,-1.f),1.f);
        aim_debt[i]=fminf(fmaxf(aim_debt[i]-s*k*aim_time,-.25f*k),.25f*k);
        const float d=fabsf(s)<.002f?0:.03f+.97f*fabsf(s);
        aim_out[i]=(uint8_t)lrintf(127.5f+(s<0?-d:d)*127.5f);
    }
    aim_time=0;
}
static void set_mouse_mode(int mode,SDL_Window *window){
    mouse_mode=mode;mouse_anchored=mouse_touch=0;spray_tick=0;memset(aim_debt,0,sizeof(aim_debt));look[0]=look[1]=0;aim_out[0]=aim_out[1]=128;aim_time=0;memcpy(spray,spray_pose,sizeof(spray));
    spray_wait_release=mode==2;
    fprintf(stderr,"PAD_MOUSE mode=%s\n",mode==1?"touchpad":mode==2?"spray":mode==3?"aim":"off");
    pad_panel_set_mode(mode);
    if(mode==2){float dx,dy;pad_panel_raw(&dx,&dy);}
    if(!window)return;
    static char base[256];
    if(!base[0])snprintf(base,sizeof(base),"%s",SDL_GetWindowTitle(window));
    char title[320];
    snprintf(title,sizeof(title),"%s%s",base,mode==1?"  [touchpad: mouse]":mode==2?"  [spray: mouse]":mode==3?"  [aim: mouse]":"");
    SDL_SetWindowTitle(window,title);
}
static uint8_t axis(int16_t v){return (uint8_t)(((int)v+32768)>>8);}
static int32_t error(unsigned n){return (int32_t)(UINT32_C(0x80920000)|n);}
static int32_t GUEST_ABI pad_init(void){
    if(!initialized){SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4,"1");
        sdl_ready=SDL_InitSubSystem(SDL_INIT_GAMEPAD);if(sdl_ready){SDL_SetGamepadEventsEnabled(false);SDL_SetJoystickEventsEnabled(false);}
        else fprintf(stderr,"PAD SDL init failed: %s\n",SDL_GetError());}
    if(!initialized)load_controls();
    initialized=1;return 0;
}
static int32_t GUEST_ABI pad_open(int user,int type,int index,const void *param){
    (void)param;if(!initialized)return error(5);if(user<0||type||index)return error(1);if(opened)return error(4);opened=1;return 1;
}
static int32_t GUEST_ABI pad_close(int handle){if(handle!=1||!opened)return error(3);opened=0;return 0;}
static int32_t GUEST_ABI pad_info(int handle,PadInfo *out){
    if(handle!=1||!opened)return error(3);if(!out)return error(1);memset(out,0,sizeof(*out));
    out->density=44.86f;out->width=1920;out->height=942;out->left_dead=10;
    {const char *aim=getenv("ISS_MOUSE_AIM");out->right_dead=(!aim||aim[0]!='0')&&!pad_look_direct?0:10;} // the mouse as stick needs none
    out->connected=out->count=1;return 0;
}
static int32_t GUEST_ABI pad_read(int handle,PadData *out){
    if(handle!=1||!opened)return error(3);if(!out)return error(1);host_lock(&lock);
    memset(out,0,sizeof(*out));out->lx=out->ly=out->rx=out->ry=128;out->orientation[3]=1;out->acceleration[1]=1;
    out->connected=out->connected_count=1;out->timestamp=runtime_process_time_us();
    int contacts[2]={0,0};uint16_t xy[2][2]={{480,471},{1440,471}};
    live.connected=0;
    read_sdl(out,contacts,xy);
    if(pad_swipe>=0){swipe_direction=pad_swipe;swipe_started=out->timestamp;was_touch[0]=0;pad_swipe=-1;}
    SDL_Window *const focus=SDL_GetKeyboardFocus();
    const int focused=focus!=NULL;
    int mouse_left=0,mouse_right=0;float mouse_dx=0,mouse_dy=0;
    if(focused){
        if(key(SDL_SCANCODE_A))out->lx=0;if(key(SDL_SCANCODE_D))out->lx=255;if(key(SDL_SCANCODE_W))out->ly=0;if(key(SDL_SCANCODE_S))out->ly=255;
        if(key(SDL_SCANCODE_J))out->rx=0;if(key(SDL_SCANCODE_L))out->rx=255;if(key(SDL_SCANCODE_I))out->ry=0;if(key(SDL_SCANCODE_K))out->ry=255;
        // The PC layout: Space jumps (Cross), Shift dashes (Circle), F melee (Square), E subdue and heal (Triangle),
        // Q and R the power's moves (L1, R1). Ctrl is L3 and M R3: their use in the game is not known.
        // O and U are the triggers while the mouse does not aim (F4).
        static const struct {SDL_Scancode code;uint32_t bit;} keys[]={{SDL_SCANCODE_SPACE,0x4000},{SDL_SCANCODE_LSHIFT,0x2000},
            {SDL_SCANCODE_F,0x8000},{SDL_SCANCODE_E,0x1000},{SDL_SCANCODE_Q,0x400},{SDL_SCANCODE_R,0x800},{SDL_SCANCODE_LCTRL,2},
            {SDL_SCANCODE_M,4},{SDL_SCANCODE_ESCAPE,8},{SDL_SCANCODE_UP,0x10},{SDL_SCANCODE_RIGHT,0x20},{SDL_SCANCODE_DOWN,0x40},{SDL_SCANCODE_LEFT,0x80}};
        for(unsigned k=0;k<sizeof(keys)/sizeof(keys[0]);++k)if(key(keys[k].code))out->buttons|=keys[k].bit;
        if(key(SDL_SCANCODE_U))out->l2=255;if(key(SDL_SCANCODE_O))out->r2=255;
        const int both=key(touch_keys[K_TOUCH_BOTH]),left=key(touch_keys[K_TOUCH_LEFT])||key(SDL_SCANCODE_TAB)||both,right=key(touch_keys[K_TOUCH_RIGHT])||both;
        const int centre_touch=key(touch_keys[K_TOUCH_CENTER]);
        if(left){contacts[0]=1;xy[0][0]=480;xy[0][1]=471;}if(right){contacts[1]=1;xy[1][0]=1440;xy[1][1]=471;}
        if(centre_touch){contacts[0]=1;xy[0][0]=960;xy[0][1]=471;}
        if(left||right||centre_touch)out->buttons|=0x100000;
        for(int i=0;i<4;++i){int down=key(touch_keys[K_SWIPE_UP+i]);if(down&&!swipe_keys[i]){swipe_direction=i;swipe_started=out->timestamp;was_touch[0]=0;}swipe_keys[i]=down;}
        SDL_Window *window=focus;game_window=window;
        const int f6=key(touch_keys[K_MOUSE]),f7=key(touch_keys[K_SPRAY]),f5=key(touch_keys[K_PANEL]),f4=key(touch_keys[K_AIM]);
        if(f6&&!mode_keys[0])set_mouse_mode(mouse_mode==1?0:1,window);
        if(f7&&!mode_keys[1])set_mouse_mode(mouse_mode==2?0:2,window);
        // The mouse aims from the first focused read (settings: mouse_aim), unless it is switched off.
        static int aim_default=-1;
        if(aim_default<0){const char *s=getenv("ISS_MOUSE_AIM");aim_default=!s||s[0]!='0';
            if(aim_default&&mouse_mode==0)set_mouse_mode(3,window);}
        if(f4&&!mode_keys[3])set_mouse_mode(mouse_mode==3?0:3,window);
        if(f5&&!mode_keys[2])pad_panel_toggle(window);
        mode_keys[0]=f6;mode_keys[1]=f7;mode_keys[2]=f5;mode_keys[3]=f4;
        const int swap=0;
        mouse_left=mouse_button(swap?SDL_BUTTON_RMASK:SDL_BUTTON_LMASK);mouse_right=mouse_button(swap?SDL_BUTTON_LMASK:SDL_BUTTON_RMASK);
        int client_w=0,client_h=0;float cursor_x,cursor_y;
        // Mouse positions are the focused game window's own (no global position on Wayland).
        if(mouse_mode&&window&&SDL_GetWindowSize(window,&client_w,&client_h)&&client_w>0&&client_h>0){
            SDL_GetMouseState(&cursor_x,&cursor_y);cursor_x=(float)(long)cursor_x;cursor_y=(float)(long)cursor_y;
            // Clicks on the touchpad panel are its own, not a finger on the game's touchpad.
            if(mouse_mode==1&&pad_panel_covers())mouse_left=mouse_right=0;
            if(mouse_mode==1){
                const int inside=cursor_x>=0&&cursor_y>=0&&cursor_x<client_w&&cursor_y<client_h;
                // A finger starts only inside the window; a drag past the edge stays on the pad's edge.
                if((mouse_left||mouse_right)&&(inside||mouse_touch)){mouse_touch=1;contacts[0]=1;
                    xy[0][0]=touch_coord((cursor_x+.5f)/(float)client_w,1919);xy[0][1]=touch_coord((cursor_y+.5f)/(float)client_h,941);
                    if(mouse_right)out->buttons|=0x100000;}
                else mouse_touch=0;
            }else{
                // The cursor is held (hidden) at the window's centre, so the can turns as far as the
                // mouse moves. The movement is the mouse's raw counts (no pointer acceleration: the
                // same hand movement always turns the can as much) when Raw Input works, else the
                // cursor's.
                const float centre_x=(float)(client_w/2),centre_y=(float)(client_h/2);
                if(mouse_anchored){mouse_dx=cursor_x-mouse_anchor_x;mouse_dy=cursor_y-mouse_anchor_y;}
                mouse_anchor_x=centre_x;mouse_anchor_y=centre_y;mouse_anchored=1;
                if(cursor_x!=centre_x||cursor_y!=centre_y)SDL_WarpMouseInWindow(window,centre_x,centre_y);
                float raw_dx,raw_dy;if(pad_panel_raw(&raw_dx,&raw_dy)){mouse_dx=raw_dx;mouse_dy=raw_dy;}
                if(mouse_mode==2&&mouse_button(SDL_BUTTON_MMASK))memcpy(spray,spray_pose,sizeof(spray));
            }
        }
    }else{memset(swipe_keys,0,sizeof(swipe_keys));swipe_direction=-1;mouse_anchored=mouse_touch=0;}
    // The touchpad window (F5): a finger where it is pressed and dragged (Shift: two, mirrored),
    // the right button clicks; its buttons press the sides, swipe and switch the modes.
    PadPanel panel;pad_panel_read(&panel);
    if(panel.touch){contacts[0]=1;xy[0][0]=touch_coord(panel.x,1919);xy[0][1]=touch_coord(panel.y,941);
        if(panel.two){contacts[1]=1;xy[1][0]=touch_coord(1-panel.x,1919);xy[1][1]=xy[0][1];}}
    if(panel.left||panel.both){contacts[0]=1;xy[0][0]=480;xy[0][1]=471;}
    if(panel.right||panel.both){contacts[1]=1;xy[1][0]=1440;xy[1][1]=471;}
    if(panel.centre){contacts[0]=1;xy[0][0]=960;xy[0][1]=471;}
    if(panel.click||panel.left||panel.right||panel.both||panel.centre){out->buttons|=0x100000;
        if(!contacts[0]&&!contacts[1]){contacts[0]=1;xy[0][0]=960;xy[0][1]=471;}}
    if(panel.options)out->buttons|=8;
    if(panel.swipe>=0){swipe_direction=panel.swipe;swipe_started=out->timestamp;was_touch[0]=0;}
    if(panel.mode>=0&&game_window)set_mouse_mode(panel.mode==mouse_mode?0:panel.mode,game_window);
    if(panel.recentre)memcpy(spray,spray_pose,sizeof(spray));
    // Scripted spray (ISS_PAD_CMD_FILE: spray, center, shake:s, paint:s, aimleft/aimright/aimup/aimdown:s): the can
    // mode toggled, shaken and turned as the mouse would.
    if(cmd_spray_toggle){cmd_spray_toggle=0;set_mouse_mode(mouse_mode==2?0:2,game_window);spray_wait_release=0;}
    if(cmd_spray_center){cmd_spray_center=0;memcpy(spray,spray_pose,sizeof(spray));}
    const double cmd_now=(double)out->timestamp/1e6;
    if(mouse_mode==2&&cmd_now<cmd_aim_until){mouse_dx+=cmd_aim_dx;mouse_dy+=cmd_aim_dy;}
    if(mouse_mode==2){
        const float dt=spray_tick?(float)(out->timestamp-spray_tick)/1e6f:0;spray_tick=out->timestamp;
        // Shift: fine aim (a third of the speed); the window's sensitivity scales both.
        const float scale=panel.sensitivity*(focused&&shift_held()?.35f:1.f);
        if(!focused&&cmd_now>=cmd_aim_until)mouse_dx=mouse_dy=0;
        if(spray_wait_release){if(!mouse_left&&!mouse_right)spray_wait_release=0;mouse_left=mouse_right=0;}
        pad_spray_step(spray,mouse_dx*scale,mouse_dy*scale,dt>0.1f?0.1f:dt,(focused&&mouse_right)||cmd_now<cmd_shake_until,(double)out->timestamp/1e6,out->angular,out->acceleration);
        memcpy(out->orientation,spray,sizeof(spray));
        if(focused&&mouse_left){out->r2=255;}
    }

    // F4: the mouse looks (the camera's angle, or the right stick: aim_step); the left button fires
    // (R2), the right button aims (L2), the middle button is R3.
    if(mouse_mode==3){
        const float dt=spray_tick?(float)(out->timestamp-spray_tick)/1e6f:0;spray_tick=out->timestamp;
        if(!focused)mouse_dx=mouse_dy=0;
        if(pad_look_direct){if(++look_unread>2)look[0]=look[1]=0;look[0]+=mouse_dx;look[1]+=mouse_dy;}
        else{aim_step(mouse_dx,mouse_dy,dt>.1f?.1f:dt);if(aim_out[0]!=128)out->rx=aim_out[0];if(aim_out[1]!=128)out->ry=aim_out[1];}
        if(focused){if(mouse_left)out->r2=255;if(mouse_right)out->l2=255;if(mouse_button(SDL_BUTTON_MMASK))out->buttons|=4;}
    }

    // ISS_PAD_REAL=0 (scripted runs): the real controller and keyboard are ignored, only the pad
    // script drives the game (a hand on the controller changed the route of A/B runs).
    static int real_pad=-1;if(real_pad<0){const char *s=getenv("ISS_PAD_REAL");real_pad=!s||s[0]!='0';}
    if(!real_pad){out->buttons=0;out->lx=out->ly=out->rx=out->ry=128;out->l2=out->r2=0;contacts[0]=contacts[1]=0;swipe_direction=-1;}
    if(swipe_direction>=0){float t=(float)(out->timestamp-swipe_started)/450000.0f;out->buttons&=~UINT32_C(0x100000);
        if(t>=1){swipe_direction=-1;contacts[0]=contacts[1]=0;}else{
            float x=.5f,y=.5f;if(swipe_direction==0)y=.85f-.7f*t;if(swipe_direction==1)y=.15f+.7f*t;
            if(swipe_direction==2)x=.85f-.7f*t;if(swipe_direction==3)x=.15f+.7f*t;
            contacts[0]=1;contacts[1]=0;xy[0][0]=touch_coord(x,1919);xy[0][1]=touch_coord(y,941);out->buttons&=~UINT32_C(0x100000);}
    }
    // ISS_PAD_SCRIPT=run (benchmarks): after ISS_PAD_SCRIPT_DELAY seconds of process time the
    // character runs forward, turning the camera for 2 s of every 12 and jumping every 3 s, so a
    // run measures the city while moving (streaming, changing shadow cascades) repeatably.
    static int script=-1;static double script_delay;
    if(script<0){const char *s=getenv("ISS_PAD_SCRIPT"),*d=getenv("ISS_PAD_SCRIPT_DELAY");
        script=!s?0:!strcmp(s,"run")?1:!strcmp(s,"pause")?2:!strcmp(s,"pausecycle")?3:!strcmp(s,"turn")?4:0;script_delay=d?atof(d):55.0;
        if(script)fprintf(stderr,"PAD_SCRIPT %s delay=%.0fs\n",script==1?"run":script==2?"pause":script==3?"pausecycle":"turn",script_delay);}
    // ISS_PAD_SCRIPT=turn (A/B of what is drawn): after the delay the character stays where it is
    // and the camera turns right slowly, so two runs see the same views at the same seconds.
    if(script==4){double t=(double)out->timestamp/1e6-script_delay;if(t>=0)out->rx=168;}
    if(script==1){double t=(double)out->timestamp/1e6-script_delay;
        if(t>=0){out->ly=0;if(fmod(t,12.0)<2.0)out->rx=200;if(fmod(t,3.0)<0.3)out->buttons|=0x4000;}}
    // ISS_PAD_SCRIPT=pause: after the delay OPTIONS is pressed once (0.3 s) and the game stays in
    // its pause menu (where the GPU hung for the user).
    if(script==2){double t=(double)out->timestamp/1e6-script_delay;
        if(t>=0&&t<0.3)out->buttons|=0x0008;}
    // ISS_PAD_SCRIPT=pausecycle: OPTIONS at the delay (pause), CIRCLE 20 s later (back to the game;
    // OPTIONS does not close the menu) and OPTIONS 40 s later (pause again, it stays).
    if(script==3){double t=(double)out->timestamp/1e6-script_delay;
        if((t>=0&&t<0.3)||(t>=40&&t<40.3))out->buttons|=0x0008;
        if(t>=20&&t<20.3)out->buttons|=0x2000;}
    // ISS_PAD_SEQ (menu tests): "t:button[:seconds],..." with t in seconds of process time and the
    // buttons options cross circle square triangle up down left right l1 r1 l2 r2 l3 r3 touch, or
    // the left stick lup ldown lleft lright (0.2 s by default). Each press is logged once.
    static int seq_loaded;static struct{double t,dur;uint32_t mask;int stick,logged;char name[12];}seq[96];static int seq_count;
    static const struct{const char *n;uint32_t m;int stick;}names[]={{"options",8,0},{"cross",0x4000,0},{"circle",0x2000,0},
            {"square",0x8000,0},{"triangle",0x1000,0},{"up",0x10,0},{"down",0x40,0},{"left",0x80,0},{"right",0x20,0},
            {"l1",0x400,0},{"r1",0x800,0},{"l2",0x100,0},{"r2",0x200,0},{"l3",2,0},{"r3",4,0},{"touch",0x100000,0},
            {"lup",0,1},{"ldown",0,2},{"lleft",0,3},{"lright",0,4}};
    if(!seq_loaded){seq_loaded=1;const char *s=getenv("ISS_PAD_SEQ");
        while(s&&*s&&seq_count<96){char item[48];size_t n=strcspn(s,",");if(n>=sizeof(item))n=sizeof(item)-1;memcpy(item,s,n);item[n]=0;s+=n;if(*s==',')++s;
            char *a=strchr(item,':');if(!a)continue;*a++=0;char *b=strchr(a,':');if(b)*b++=0;
            for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);++i)if(!strcmp(a,names[i].n)){
                seq[seq_count].t=atof(item);seq[seq_count].dur=b?atof(b):0.2;seq[seq_count].mask=names[i].m;seq[seq_count].stick=names[i].stick;
                strncpy(seq[seq_count].name,a,sizeof(seq[0].name)-1);++seq_count;break;}}
        if(seq_count)fprintf(stderr,"PAD_SEQ %d presses\n",seq_count);}
    // ISS_PAD_CMD_FILE (scripted menu walks): the same "button[:seconds]" items, read from this file
    // when it appears (checked every 100 ms) and pressed one after the other from then on, 0.25 s
    // apart; the file is deleted once read. "wait:s" only waits.
    {static const char *cmd_path;static int cmd_init;static uint64_t cmd_checked;static double cmd_next;
        if(!cmd_init){cmd_init=1;cmd_path=getenv("ISS_PAD_CMD_FILE");}
        const double now=(double)out->timestamp/1e6;
        if(cmd_path&&out->timestamp-cmd_checked>=100000){cmd_checked=out->timestamp;
            FILE *f=fopen(cmd_path,"rb");char text[512]={0};
            if(f){size_t n=fread(text,1,sizeof(text)-1,f);text[n]=0;fclose(f);unlink(cmd_path);
                if(cmd_next<now)cmd_next=now;
                const char *s=text;
                while(*s){char item[48];size_t k=strcspn(s,",\r\n");if(k>=sizeof(item))k=sizeof(item)-1;memcpy(item,s,k);item[k]=0;s+=k;while(*s==','||*s=='\r'||*s=='\n')++s;
                    char *b=strchr(item,':');if(b)*b++=0;const double dur=b?atof(b):0.2;
                    if(!strcmp(item,"wait")){cmd_next+=dur;continue;}
                    if(!strcmp(item,"spray")){cmd_spray_toggle=1;continue;}
                    if(!strcmp(item,"center")){cmd_spray_center=1;continue;}
                    if(!strcmp(item,"paint")){cmd_paint_until=cmd_next+dur;continue;} // R2 held while the next items run
                    if(!strcmp(item,"shake")){cmd_shake_until=cmd_next+dur;cmd_next+=dur+0.25;continue;}
                    if(!strncmp(item,"aim",3)){const float step=4.f;cmd_aim_dx=!strcmp(item,"aimleft")?-step:!strcmp(item,"aimright")?step:0;
                        cmd_aim_dy=!strcmp(item,"aimup")?-step:!strcmp(item,"aimdown")?step:0;cmd_aim_until=cmd_next+dur;cmd_next+=dur+0.25;continue;}
                    for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);++i)if(!strcmp(item,names[i].n)){
                        if(seq_count==96){int kept=0;for(int j=0;j<seq_count;++j)if(seq[j].t+seq[j].dur>=now)seq[kept++]=seq[j];seq_count=kept;}
                        if(seq_count<96){seq[seq_count].t=cmd_next;seq[seq_count].dur=dur;seq[seq_count].mask=names[i].m;seq[seq_count].stick=names[i].stick;
                            seq[seq_count].logged=0;memset(seq[seq_count].name,0,sizeof(seq[0].name));strncpy(seq[seq_count].name,item,sizeof(seq[0].name)-1);++seq_count;cmd_next+=dur+0.25;}
                        break;}}
                fprintf(stderr,"PAD_CMD %s",text);if(text[0]&&text[strlen(text)-1]!='\n')fputc('\n',stderr);}}}
    for(int i=0;i<seq_count;++i){double t=(double)out->timestamp/1e6;if(t<seq[i].t||t>=seq[i].t+seq[i].dur)continue;
        out->buttons|=seq[i].mask;if(seq[i].mask==0x100)out->l2=255;if(seq[i].mask==0x200)out->r2=255;if(seq[i].stick==1)out->ly=0;if(seq[i].stick==2)out->ly=255;if(seq[i].stick==3)out->lx=0;if(seq[i].stick==4)out->lx=255;
        if(seq[i].mask==0x100000&&!contacts[0]&&!contacts[1]){contacts[0]=1;xy[0][0]=960;xy[0][1]=471;}
        if(!seq[i].logged){seq[i].logged=1;fprintf(stderr,"PAD_SEQ %s at %.2f s\n",seq[i].name,t);}}
    if((double)out->timestamp/1e6<cmd_paint_until)out->r2=255;
    if(out->l2>30)out->buttons|=0x100;if(out->r2>30)out->buttons|=0x200;
    memcpy(live.contacts,contacts,sizeof(contacts));memcpy(live.xy,xy,sizeof(xy));live.click=(out->buttons&0x100000)!=0;pad_panel_report(&live);
    if((contacts[0]||contacts[1])&&!was_touch[0]&&!was_touch[1])touch_start=out->timestamp;
    for(unsigned i=0;i<2;++i){if(contacts[i]){
        if(!was_touch[i]){contact_id[i]=next_touch;next_touch=(uint8_t)(next_touch==127?1:next_touch+1);}
        Touch *t=&out->touch[out->touch_count++];t->x=xy[i][0];t->y=xy[i][1];t->id=contact_id[i];
    }was_touch[i]=contacts[i];}
    if(out->touch_count)out->held=(uint32_t)(out->timestamp-touch_start);
    // The save data dialog (src/save_dialog.c) takes the controller while it is up, as the
    // console's system dialogs do: the game reads an idle pad.
    if(save_dialog_blocking()){const uint64_t ts=out->timestamp;memset(out,0,sizeof(*out));
        out->lx=out->ly=out->rx=out->ry=128;out->orientation[3]=1;out->acceleration[1]=1;
        out->connected=out->connected_count=1;out->timestamp=ts;}
    host_unlock(&lock);return 0;
}
static int32_t GUEST_ABI pad_vibration(int handle,const uint8_t *motors){
    if(handle!=1||!opened)return error(3);if(!motors)return error(1);
    host_lock(&lock);
    if(sdl_pad)SDL_RumbleGamepad(sdl_pad,(Uint16)(motors[0]*257),(Uint16)(motors[1]*257),1000);
    host_unlock(&lock);return 0;
}
static uint8_t lightbar[4];
static int32_t GUEST_ABI pad_light(int handle,const void *rgb){if(handle!=1||!opened)return error(3);if(!rgb)return error(1);host_lock(&lock);memcpy(lightbar,rgb,4);if(sdl_pad)SDL_SetGamepadLED(sdl_pad,lightbar[0],lightbar[1],lightbar[2]);host_unlock(&lock);return 0;}
uintptr_t pad_resolve(const char *identity){
    if(!strstr(identity,"#libScePad#1#libScePad#257"))return 0;const char *api=runtime_api_name(identity);
#define API(n,f) if(!strcmp(api,n))return (uintptr_t)f
    API("scePadInit",pad_init);API("scePadOpen",pad_open);API("scePadClose",pad_close);API("scePadGetControllerInformation",pad_info);
    API("scePadReadState",pad_read);API("scePadSetVibration",pad_vibration);API("scePadSetLightBar",pad_light);
#undef API
    return 0;
}
