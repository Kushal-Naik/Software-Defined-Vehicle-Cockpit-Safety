/*
* ╔══════════════════════════════════════════════════════════════════╗
* ║                  MULTI-PROCESS QUAD-CORE ARCHITECTURE            ║
* ║  ──────────────────────────────────────────────────────────────  ║
* ║  CORE 0: Guardian Process (Actuators)                            ║
* ║  CORE 1: HAM Watchdog + CLI Terminal                             ║
* ║  CORE 2: Sensor Hub (Touch, PIR, Sonar, MPU)                     ║
* ║  CORE 3: Infotainment (OLED, Prio 10) + Hall Telemetry (Prio 50) ║
* ╚══════════════════════════════════════════════════════════════════╝
*/
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <pthread.h>
#include <sys/neutrino.h>
#include <sys/mman.h>
#include <sys/syspage.h>
#include <sys/wait.h>
#include <sys/dispatch.h>
#include <hw/inout.h>
#include <hw/i2c.h>
#include <devctl.h>
/*Memory Map & Pins*/
#define BCM2711_GPIO_BASE 0xFE200000UL
#define GPIO_LEN          0xB4
#define GPSET0            0x1C
#define GPCLR0            0x28
#define GPLEV0            0x34
#define TRIG_PIN    23
#define ECHO_PIN    24
#define PIR_PIN     25
#define TOUCH_PIN   18
#define HALL_PIN    17
#define RELAY_PIN   27
#define BUZZER_PIN  22
#define CRASH_LED   5
/*Constants & Thresholds*/
#define I2C_DEVICE       "/dev/i2c1"
#define MPU_ADDR         0x68
#define OLED_ADDR        0x3C
#define OLED_W           128
#define OLED_H           64
#define NUM_STARS        20
#define MPU_CRASH_THRESH 25000
#define HALL_RPM_THRESH  300
/* IPC Names & Pulses */
#define IPC_GUARDIAN_NAME    "aegis_guard"
#define IPC_SENSOR_NAME      "aegis_sense"
#define PULSE_TOGGLE_MOTOR   10
#define PULSE_EMERGENCY_STOP 20
#define PULSE_CRASH_DETECT   21
#define PULSE_DISARM_GUARD   30
#define PULSE_DISARM_PIR     31
#define PULSE_DISARM_ULTRA   32
#define PULSE_DISARM_MPU     33
static uintptr_t g_gpio;
static uint64_t  g_cps;
/*  Hardware Helpers  */
static void _gpio_mode(int pin, int out) {
  int reg = pin / 10;
  int shift = (pin % 10) * 3;
  uint32_t v = in32(g_gpio + (reg * 4));
  v &= ~(7u << shift);
  if (out) v |= (1u << shift);
  out32(g_gpio + (reg * 4), v);
}
static inline void _hi(int p) { out32(g_gpio + GPSET0, 1u << p); }
static inline void _lo(int p) { out32(g_gpio + GPCLR0, 1u << p); }
static inline int  _rd(int p) { return (int)((in32(g_gpio + GPLEV0) >> p) & 1u); }
static void _bind(uint32_t core_mask, int prio) {
  ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)core_mask);
  struct sched_param sp = { .sched_priority = prio };
  pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
}
static void map_hw() {
  if (ThreadCtl(_NTO_TCTL_IO, 0) == -1) exit(1);
  g_gpio = mmap_device_io(GPIO_LEN, BCM2711_GPIO_BASE);
  g_cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
}
int i2c_w2(int fd, uint8_t addr, uint8_t reg, uint8_t val) {
  struct { i2c_send_t h; uint8_t d[2]; } m;
  m.h.slave.addr = addr; m.h.slave.fmt = I2C_ADDRFMT_7BIT;
  m.h.len = 2; m.h.stop = 1; m.d[0] = reg; m.d[1] = val;
  return devctl(fd, DCMD_I2C_SEND, &m, sizeof(m.h) + 2, NULL);
}
int i2c_rn(int fd, uint8_t addr, uint8_t reg, uint8_t *buf, int n) {
  struct { i2c_sendrecv_t h; uint8_t d[16]; } sr;
  sr.h.slave.addr = addr; sr.h.slave.fmt = I2C_ADDRFMT_7BIT;
  sr.h.send_len = 1; sr.h.recv_len = n; sr.h.stop = 1; sr.d[0] = reg;
  int rc = devctl(fd, DCMD_I2C_SENDRECV, &sr, sizeof(sr), NULL);
  if (rc == EOK) { for(int i=0; i<n; i++) buf[i] = sr.d[i]; }
  return rc;
}
double get_dist() {
  _lo(TRIG_PIN); usleep(2); _hi(TRIG_PIN); usleep(10); _lo(TRIG_PIN);
  uint64_t t0, t1; int to = 0;
  while(!_rd(ECHO_PIN)) if(++to > 50000) return -1;
  t0 = ClockCycles();
  to = 0;
  while(_rd(ECHO_PIN)) if(++to > 100000) return -1;
  t1 = ClockCycles();
  return ((double)(t1 - t0) / g_cps * 1e6) * 0.0343 / 2.0;
}
/*
PROCESS 1: CORE 0 GUARDIAN
*/
static void guardian_proc() {
  map_hw(); _bind(0x01, 63);
  _gpio_mode(RELAY_PIN, 1); _gpio_mode(BUZZER_PIN, 1); _gpio_mode(CRASH_LED, 1);
  _lo(RELAY_PIN); _lo(BUZZER_PIN); _lo(CRASH_LED);
  name_attach_t *att = NULL;
  for(int i=0; i<10; i++) { att = name_attach(NULL, IPC_GUARDIAN_NAME, 0); if(att) break; usleep(200000); }
  int motor_running = 0; int is_armed = 1; struct _pulse p;
  while (1) {
      MsgReceivePulse(att->chid, &p, sizeof(p), NULL);
      if (p.code == PULSE_TOGGLE_MOTOR) {
          motor_running = !motor_running;
          if (motor_running) { _hi(RELAY_PIN); } else { _lo(RELAY_PIN); }
      }
      else if (p.code == PULSE_EMERGENCY_STOP && motor_running && is_armed) {
          motor_running = 0; _lo(RELAY_PIN);
          printf("\n[CORE 0] ⚠️ EMERGENCY STOP!\nAEGIS > "); fflush(stdout);
          _hi(BUZZER_PIN); usleep(500000); _lo(BUZZER_PIN);
      }
      else if (p.code == PULSE_CRASH_DETECT && motor_running && is_armed) {
          motor_running = 0; _lo(RELAY_PIN);
          printf("\n[CORE 0] 💥 CRASH DETECTED! Deploying Airbags.\nAEGIS > "); fflush(stdout);
          _hi(CRASH_LED); _hi(BUZZER_PIN); usleep(2000000); _lo(CRASH_LED); _lo(BUZZER_PIN);
      }
      else if (p.code == PULSE_DISARM_GUARD) {
          is_armed = !is_armed;
          printf("\n[CORE 0] Guardian is now %s\nAEGIS > ", is_armed ? "ARMED" : "DISARMED"); fflush(stdout);
      }
  }
}
/*
PROCESS 2: CORE 2 SENSOR HUB (No Hall Effect here anymore)
*/
volatile int pir_armed = 1, ultra_armed = 1, mpu_armed = 1;
void *sensor_hw_thread(void *arg) {
  _bind(0x04, 55);
  int coid = -1, touch_prev = _rd(TOUCH_PIN), mpu_tick = 0;
  int16_t last_ax = 0, last_ay = 0, last_az = 0;
  int i2c_fd = open(I2C_DEVICE, O_RDWR);
  if (i2c_fd >= 0) i2c_w2(i2c_fd, MPU_ADDR, 0x6B, 0x00);
  while (1) {
      if (coid == -1) coid = name_open(IPC_GUARDIAN_NAME, 0);
      int touch_now = _rd(TOUCH_PIN);
      if (touch_now == 1 && touch_prev == 0) {
          if (coid != -1) MsgSendPulse(coid, 55, PULSE_TOGGLE_MOTOR, 0);
          usleep(200000);
      }
      touch_prev = touch_now;
      if (_rd(RELAY_PIN) && coid != -1) {
          double dist = get_dist();
          int pir = _rd(PIR_PIN);
          if ((pir_armed && pir) || (ultra_armed && dist > 0 && dist < 20.0)) {
              MsgSendPulse(coid, 63, PULSE_EMERGENCY_STOP, 0);
              usleep(1000000);
          }
          if (mpu_armed && i2c_fd >= 0 && (mpu_tick++ >= 5)) {
              mpu_tick = 0;
              uint8_t raw[6];
              if (i2c_rn(i2c_fd, MPU_ADDR, 0x3B, raw, 6) == EOK) {
                  int16_t ax = (raw[0]<<8) | raw[1], ay = (raw[2]<<8) | raw[3], az = (raw[4]<<8) | raw[5];
                  if (last_ax != 0 && (abs(ax-last_ax) > MPU_CRASH_THRESH || abs(ay-last_ay) > MPU_CRASH_THRESH || abs(az-last_az) > MPU_CRASH_THRESH)) {
                      MsgSendPulse(coid, 63, PULSE_CRASH_DETECT, 0); usleep(2000000);
                  }
                  last_ax = ax; last_ay = ay; last_az = az;
              }
          }
      }
      usleep(10000);
  }
  return NULL;
}
static void sensor_proc() {
  map_hw(); _bind(0x04, 60);
  _gpio_mode(TRIG_PIN, 1); _gpio_mode(ECHO_PIN, 0);
  _gpio_mode(PIR_PIN, 0);  _gpio_mode(TOUCH_PIN, 0);
  pthread_t hw_t; pthread_create(&hw_t, NULL, sensor_hw_thread, NULL);
  name_attach_t *att = NULL;
  for(int i=0; i<10; i++) { att = name_attach(NULL, IPC_SENSOR_NAME, 0); if(att) break; usleep(200000); }
  struct _pulse p;
  while(1) {
      MsgReceivePulse(att->chid, &p, sizeof(p), NULL);
      if (p.code == PULSE_DISARM_PIR) { pir_armed = !pir_armed; printf("\n[CORE 2] PIR %s\nAEGIS > ", pir_armed ? "ARMED" : "DISARMED"); fflush(stdout); }
      else if (p.code == PULSE_DISARM_ULTRA) { ultra_armed = !ultra_armed; printf("\n[CORE 2] Sonar %s\nAEGIS > ", ultra_armed ? "ARMED" : "DISARMED"); fflush(stdout); }
      else if (p.code == PULSE_DISARM_MPU) { mpu_armed = !mpu_armed; printf("\n[CORE 2] MPU %s\nAEGIS > ", mpu_armed ? "ARMED" : "DISARMED"); fflush(stdout); }
  }
}
/*
PROCESS 3A: CORE 3 HALL EFFECT TELEMETRY (High Priority: 50)
*/
static void hall_proc() {
  map_hw();
  _bind(0x08, 50); /* Core 3, High Priority Preemption */
  _gpio_mode(HALL_PIN, 0);
  int hall_prev = _rd(HALL_PIN);
  int hall_edge_count = 0;
  int tick_counter = 0;
  int hall_motor_state = 0;
  while (1) {
      int hall_now = _rd(HALL_PIN);
      /* Falling edge (magnet detected) */
      if (hall_now == 0 && hall_prev == 1) hall_edge_count++;
      hall_prev = hall_now;
      /* 1-Second Timer (1000 ticks of 1ms) */
      if (++tick_counter >= 1000) {
          int rpm = hall_edge_count * 60;
          int new_state = (rpm > HALL_RPM_THRESH) ? 1 : 0;
          /* Only trigger if state crosses the 300 RPM threshold */
          if (new_state != hall_motor_state) {
              if (new_state == 1) {
                  printf("\n[CORE 3] ⚙️ HALL SENSOR: Motor is RUNNING (%d RPM)\nAEGIS > ", rpm);
              } else {
                  printf("\n[CORE 3] ⚙️ HALL SENSOR: Motor is STOPPED (%d RPM)\nAEGIS > ", rpm);
              }
              fflush(stdout);
              hall_motor_state = new_state;
          }
          hall_edge_count = 0;
          tick_counter = 0;
      }
      /* 1000Hz Fast-Poll to accurately catch high-speed magnets */
      usleep(1000);
  }
}
/*
PROCESS 3B: CORE 3 INFOTAINMENT (STRICT 40 FPS OLED ANIMATION)
*/
static uint8_t fb[8][OLED_W];
static struct { int x, y, z; } stars[NUM_STARS];
void oled_cmd(int fd, uint8_t c) { i2c_w2(fd, OLED_ADDR, 0x00, c); }
void oled_flush(int fd) {
  struct { i2c_send_t h; uint8_t d[OLED_W + 1]; } m;
  m.h.slave.addr = OLED_ADDR; m.h.slave.fmt = I2C_ADDRFMT_7BIT; m.h.len = OLED_W + 1; m.h.stop = 1; m.d[0] = 0x40;
  for (int pg = 0; pg < 8; pg++) {
      oled_cmd(fd, 0xB0 + pg); oled_cmd(fd, 0x00); oled_cmd(fd, 0x10);
      memcpy(&m.d[1], fb[pg], OLED_W);
      devctl(fd, DCMD_I2C_SEND, &m, sizeof(m.h) + OLED_W + 1, NULL);
  }
}
void px(int x, int y, int on) { if (x >= 0 && x < OLED_W && y >= 0 && y < OLED_H) { if (on) fb[y/8][x] |= (1 << (y % 8)); else fb[y/8][x] &= ~(1 << (y % 8)); } }
void rect(int x, int y, int w, int h, int on) { for (int i = x; i < x + w; i++) for (int j = y; j < y + h; j++) px(i, j, on); }
void draw_qnx(int ox, int oy) {
  rect(ox,oy,10,14,1); rect(ox+3,oy+3,4,8,0); rect(ox+6,oy+10,6,6,1); rect(ox+6,oy+10,3,3,0);
  rect(ox+16,oy,3,14,1); rect(ox+19,oy+2,3,3,1); rect(ox+22,oy+5,3,4,1); rect(ox+25,oy+9,3,3,1);
  rect(ox+28,oy,3,14,1); rect(ox+37,oy,3,4,1); rect(ox+40,oy+4,3,3,1); rect(ox+43,oy+7,5,3,1);
  rect(ox+40,oy+10,3,3,1); rect(ox+37,oy+13,3,4,1); rect(ox+51,oy,3,4,1); rect(ox+48,oy+4,3,3,1);
  rect(ox+48,oy+10,3,3,1); rect(ox+51,oy+13,3,4,1);
}
static void oled_proc() {
  _bind(0x08, 10); /* CORE 3, Low Priority */
  int fd = open(I2C_DEVICE, O_RDWR); if (fd < 0) exit(1);
  const uint8_t init_seq[] = { 0xAE,0xD5,0x80,0xA8,0x3F,0xD3,0x00,0x40,0x8D,0x14,0x20,0x02,0xA1,0xC8,0xDA,0x12,0x81,0xCF,0xD9,0xF1,0xDB,0x40,0xA4,0xA6,0xAF };
  for (int i = 0; i < sizeof(init_seq); i++) oled_cmd(fd, init_seq[i]);
  for (int i = 0; i < NUM_STARS; i++) { stars[i].x = (rand() % 200)-100; stars[i].y = (rand() % 100)-50; stars[i].z = (rand() % 150)+1; }
  int frame = 0;
  struct timespec t_start, t_end;
  const long TARGET_FRAME_NS = 25000000L; /* Exactly 25ms per frame = 40 FPS */
  while (1) {
      /* Mark start time of frame */
      clock_gettime(CLOCK_MONOTONIC, &t_start);
      memset(fb, 0, sizeof(fb));
      /* Render Starfield */
      for (int i = 0; i < NUM_STARS; i++) {
          stars[i].z -= 4;
          if (stars[i].z <= 0) {
              stars[i].x = (rand() % 200)-100;
              stars[i].y = (rand() % 100)-50;
              stars[i].z = 150;
          }
          int sx = (stars[i].x * 64 / stars[i].z) + 64;
          int sy = (stars[i].y * 64 / stars[i].z) + 32;
          /* Bounds check before drawing to prevent memory corruption */
          if (sx >= 0 && sx < OLED_W && sy >= 0 && sy < OLED_H) {
              if (stars[i].z < 40) rect(sx, sy, 2, 2, 1);
              else px(sx, sy, 1);
          }
      }
      /* Render QNX Logo */
      if (frame % 60 > 5) draw_qnx(37, 24);
      /* Push buffer to hardware */
      oled_flush(fd);
      frame++;
      /* Calculate exact elapsed time */
      clock_gettime(CLOCK_MONOTONIC, &t_end);
      long elapsed_ns = (t_end.tv_sec - t_start.tv_sec) * 1000000000L + (t_end.tv_nsec - t_start.tv_nsec);
      /* Sleep only for the remaining time required to hit 25ms */
      if (elapsed_ns < TARGET_FRAME_NS) {
          long sleep_us = (TARGET_FRAME_NS - elapsed_ns) / 1000;
          usleep(sleep_us);
      } else {
          /* if frame took > 25ms, yield instantly to avoid blocking */
          sched_yield();
      }
  }
}
/*
MAIN: HAM WATCHDOG & C2 CLI (CORE 1)
*/
pid_t g_pid, s_pid, o_pid, h_pid;
void *ham_watchdog(void *arg) {
  while (1) {
      int status; pid_t died = waitpid(-1, &status, 0);
      if (died == g_pid) { sleep(1); g_pid = fork(); if (g_pid == 0) { guardian_proc(); exit(0); } }
      else if (died == s_pid) { sleep(1); s_pid = fork(); if (s_pid == 0) { sensor_proc(); exit(0); } }
      else if (died == o_pid) { sleep(1); o_pid = fork(); if (o_pid == 0) { oled_proc(); exit(0); } }
      else if (died == h_pid) { sleep(1); h_pid = fork(); if (h_pid == 0) { hall_proc(); exit(0); } }
  }
  return NULL;
}
void fire_pulse(const char *target, int code) {
  int coid = name_open(target, 0);
  if (coid != -1) { MsgSendPulse(coid, 60, code, 0); name_close(coid); }
}
int main() {
  if (ThreadCtl(_NTO_TCTL_IO, 0) == -1) return EXIT_FAILURE;
  _bind(0x02, 10);
  g_pid = fork(); if (g_pid == 0) { guardian_proc(); exit(0); }
  s_pid = fork(); if (s_pid == 0) { sensor_proc(); exit(0); }
  o_pid = fork(); if (o_pid == 0) { oled_proc(); exit(0); }
  h_pid = fork(); if (h_pid == 0) { hall_proc(); exit(0); }
  pthread_t ham_t; pthread_create(&ham_t, NULL, ham_watchdog, NULL);
  sleep(1);
   printf(" AEGIS QUAD-CORE ACTIVE | TYPE COMMAND + ENTER         \n");
  printf(" [p] Toggle PIR Sensor                                 \n");
  printf(" [u] Toggle Ultrasonic Sensor                          \n");
  printf(" [m] Toggle MPU6050 Crash Sensor                       \n");
  printf("  [g] Toggle Guardian Safety (Ignore Everything)        \n");
  printf(" [k] Kill Sensor Process (Test HAM Resurrect)          \n");
  while (1) {
      printf("AEGIS > "); char cmd = getchar(); while (getchar() != '\n');
      switch (cmd) {
          case 'p': fire_pulse(IPC_SENSOR_NAME, PULSE_DISARM_PIR); break;
          case 'u': fire_pulse(IPC_SENSOR_NAME, PULSE_DISARM_ULTRA); break;
          case 'm': fire_pulse(IPC_SENSOR_NAME, PULSE_DISARM_MPU); break;
          case 'g': fire_pulse(IPC_GUARDIAN_NAME, PULSE_DISARM_GUARD); break;
          case 'k': kill(s_pid, SIGKILL); printf("\n[CLI] Assassinating Sensor Hub...\n"); break;
      }
  }
  return 0;
}

