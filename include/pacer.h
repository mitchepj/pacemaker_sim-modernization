/* ------------------------------------------------------------------------
 * pacer.h
 *
 * FAKE / SYNTHETIC pacemaker firmware simulator - shared header.
 * NOT A REAL MEDICAL DEVICE. See README.md.
 *
 * This header is intentionally written in an old embedded-C style:
 * Hungarian-ish prefixes, lots of raw magic numbers, one giant grab bag
 * of global state, typedefs that shadow stdint types for "portability"
 * (the way a lot of pre-C99 embedded codebases did), and prototypes for
 * functions that all take/return loosely related things.
 *
 * DO NOT clean this up before handing it to a refactoring agent - that's
 * the point of the file.
 * ------------------------------------------------------------------------
 */

#ifndef PACER_H
#define PACER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

/* ---- old-school portability typedefs (kept even though stdint exists,
 *      because that's exactly what real legacy firmware headers do) ---- */
typedef unsigned char   BYTE;
typedef unsigned short  WORD;
typedef unsigned long   DWORD;
typedef int             BOOL;
typedef signed short    INT16;
typedef unsigned short  UINT16;
typedef signed long     INT32;
typedef unsigned long   UINT32;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

/* ---- build / version stamp, nobody ever bumps the date ---- */
#define FW_MAJOR         4
#define FW_MINOR         12
#define FW_PATCH         3
#define FW_BUILD_DATE    "2011-06-30"   /* yes, this is stale on purpose */

/* ---- misc magic numbers used all over the place, some duplicated
 *      locally in .c files instead of being referenced from here ---- */
#define MAX_LOG_ENTRIES        256
#define MAX_NVRAM_PARAMS       64
#define TELEMETRY_BUF_SZ       128
#define EGM_HISTORY_LEN        512
#define NAME_BUF_SZ            32
#define CRC16_POLY             0xA001
#define NVRAM_MAGIC            0x5A5A
#define NVRAM_FILE             "pacer_nvram.bin"

/* Timing cycle defaults, all in milliseconds. Real devices spec these in
 * a mix of ms and bpm depending on the era of the datasheet; we keep that
 * inconsistency here on purpose. */
#define DEFAULT_LRL_MS         1000    /* lower rate limit -> 60 ppm */
#define DEFAULT_URL_MS         461     /* upper rate limit -> ~130 ppm */
#define DEFAULT_AVI_MS         150
#define DEFAULT_PVARP_MS       250
#define DEFAULT_VRP_MS         200
#define DEFAULT_ARP_MS         150
#define DEFAULT_PACE_WIDTH_MS  1       /* pulse width */
#define DEFAULT_PACE_AMPL_MV   3500    /* pulse amplitude, millivolts */
#define DEFAULT_ATR_RATE_BPM   180     /* atrial tachy detection rate */
#define HYSTERESIS_MS          0

/* sensing thresholds, millivolts, again inconsistent with the "MV"
 * naming used elsewhere for amplitude */
#define A_SENSE_THRESH_MV      250
#define V_SENSE_THRESH_MV      500
#define NOISE_FLOOR_MV         50

/* battery model constants */
#define BATT_NOMINAL_MV        2800
#define BATT_ERI_MV            2500     /* elective replacement indicator */
#define BATT_EOL_MV            2200     /* end of life */
#define BATT_DECAY_PER_TICK    0        /* computed dynamically, see battery.c */
#define LEAD_IMPEDANCE_LOW     200      /* ohms */
#define LEAD_IMPEDANCE_HIGH    3000     /* ohms */

/* ---- pacing modes, NASPE/BPEG-ish 3-4 letter codes packed as an enum.
 *      Kept as an enum AND as parallel string tables in modes.c - a
 *      classic legacy duplication smell. ---- */
typedef enum {
    MODE_AOO = 0,
    MODE_VOO,
    MODE_AAI,
    MODE_VVI,
    MODE_AAT,
    MODE_VVT,
    MODE_AAIR,
    MODE_VVIR,
    MODE_VDD,
    MODE_DDI,
    MODE_DDD,
    MODE_DDDR,
    MODE_COUNT
} PaceMode_t;

/* chamber/event flags, bitfield style, mixed with plain enums elsewhere */
#define EVT_NONE        0x00
#define EVT_A_PACE      0x01
#define EVT_A_SENSE     0x02
#define EVT_V_PACE      0x04
#define EVT_V_SENSE     0x08
#define EVT_A_REFRACTORY 0x10
#define EVT_V_REFRACTORY 0x20
#define EVT_NOISE       0x40
#define EVT_MODE_SWITCH 0x80

/* core timing-cycle state machine states. Some of these are only used
 * in pacer_core.c but declared globally because that's how the original
 * (fictional) codebase organized things. */
typedef enum {
    ST_INIT = 0,
    ST_WAIT_LRL,
    ST_A_PACE,
    ST_AVI_WAIT,
    ST_V_PACE,
    ST_VRP,
    ST_PVARP,
    ST_ARP,
    ST_MODE_SWITCHED,
    ST_FAULT
} PacerState_t;

/* one row of the fake intracardiac electrogram history buffer */
typedef struct {
    INT16  a_mv;
    INT16  v_mv;
    UINT32 t_ms;
} EgmSample_t;

/* one log/event entry - deliberately loose, "flags" doubles as both a
 * bitmask of EVT_* and sometimes gets treated as a plain state number
 * by older code paths (kept for authenticity) */
typedef struct {
    UINT32 t_ms;
    BYTE   flags;
    INT16  a_mv;
    INT16  v_mv;
    BYTE   mode;
} LogEntry_t;

/* NVRAM "EEPROM" parameter block - packed struct, no versioning beyond
 * the magic number, CRC appended separately in eeprom.c */
#pragma pack(push, 1)
typedef struct {
    WORD  magic;
    BYTE  mode;
    WORD  lrl_ms;
    WORD  url_ms;
    WORD  avi_ms;
    WORD  pvarp_ms;
    WORD  vrp_ms;
    WORD  arp_ms;
    WORD  pace_ampl_mv;
    BYTE  pace_width_ms;
    WORD  a_sense_thresh_mv;
    WORD  v_sense_thresh_mv;
    BYTE  rate_resp_enabled;
    BYTE  reserved[9];   /* padding for "future use", never used */
} NvramParams_t;
#pragma pack(pop)

/* -------------------------------------------------------------------
 * GLOBAL STATE. Yes, all of it. This is intentional - a real modular
 * design would pass a context struct around; the legacy code instead
 * reaches through these externs from every .c file.
 * ------------------------------------------------------------------- */
extern PacerState_t   g_state;
extern PaceMode_t     g_mode;
extern UINT32         g_tick_ms;
extern UINT32         g_last_a_evt_ms;
extern UINT32         g_last_v_evt_ms;
extern BOOL           g_a_refractory;
extern BOOL           g_v_refractory;
extern BOOL           g_mode_switched;
extern UINT32         g_ms_since_boot;

extern WORD           g_lrl_ms;
extern WORD           g_url_ms;
extern WORD           g_avi_ms;
extern WORD           g_pvarp_ms;
extern WORD           g_vrp_ms;
extern WORD           g_arp_ms;
extern WORD           g_pace_ampl_mv;
extern BYTE           g_pace_width_ms;
extern WORD           g_a_sense_thresh_mv;
extern WORD           g_v_sense_thresh_mv;
extern BYTE           g_rate_resp_enabled;

extern DWORD          g_batt_mv;
extern DWORD          g_lead_a_impedance;
extern DWORD          g_lead_v_impedance;
extern BOOL           g_eri_flag;
extern BOOL           g_eol_flag;

extern EgmSample_t    g_egm_hist[EGM_HISTORY_LEN];
extern UINT32         g_egm_hist_idx;

extern LogEntry_t     g_log[MAX_LOG_ENTRIES];
extern UINT32         g_log_count;
extern UINT32         g_log_head;   /* ring buffer head, wraps */

extern BYTE           g_telemetry_buf[TELEMETRY_BUF_SZ];
extern UINT32         g_telemetry_len;

extern UINT32         g_atr_counter;       /* fake atrial tachy counter */
extern BOOL           g_ams_active;        /* automatic mode switch active */
extern PaceMode_t     g_pre_ams_mode;

extern int            g_verbose;           /* CLI verbosity flag */

/* -------------------------------------------------------------------
 * Prototypes. Grouped roughly by file but not enforced - several of
 * these functions reach into globals owned "by" another module, which
 * is realistic for this kind of codebase.
 * ------------------------------------------------------------------- */

/* pacer_core.c */
void pacer_core_init(void);
void pacer_core_tick(UINT32 dt_ms);
int  pacer_core_handle_state(void);
void pacer_core_reset_cycle(void);
const char *pacer_state_name(PacerState_t st);

/* sensing.c */
void sensing_init(void);
void sensing_generate_sample(EgmSample_t *out);
int  sensing_check_atrial(INT16 mv);
int  sensing_check_ventricular(INT16 mv);
void sensing_push_history(EgmSample_t *s);
int  sensing_is_noise(INT16 mv, INT16 thresh);

/* modes.c */
const char *mode_to_string(PaceMode_t m);
PaceMode_t  mode_from_string(const char *s);
int  mode_is_atrial_paced(PaceMode_t m);
int  mode_is_ventricular_paced(PaceMode_t m);
int  mode_is_atrial_sensed(PaceMode_t m);
int  mode_is_ventricular_sensed(PaceMode_t m);
int  mode_is_rate_responsive(PaceMode_t m);
int  mode_is_dual_chamber(PaceMode_t m);
void mode_apply_defaults(PaceMode_t m);
int  mode_step(PaceMode_t m, BYTE evt_flags);

/* telemetry.c */
void telemetry_init(void);
int  telemetry_build_packet(BYTE evt_flags);
void telemetry_flush(void);
UINT16 telemetry_checksum(const BYTE *buf, int len);
void telemetry_log_event(BYTE evt_flags, INT16 a_mv, INT16 v_mv);
void telemetry_dump_log(void);

/* battery.c */
void battery_init(void);
void battery_tick(UINT32 dt_ms, int paced_this_tick);
int  battery_check_eri(void);
int  battery_check_eol(void);
DWORD lead_impedance_sample(int channel);

/* eeprom.c */
UINT16 crc16_calc(const BYTE *data, int len);
int  eeprom_load(void);
int  eeprom_save(void);
void eeprom_defaults(NvramParams_t *p);
int  eeprom_validate(const NvramParams_t *p, UINT16 stored_crc);

/* arrhythmia.c */
void arrhythmia_init(void);
int  arrhythmia_check_atrial_tachy(UINT32 interval_ms);
void arrhythmia_run_ams_logic(void);
void arrhythmia_reset_counters(void);

#endif /* PACER_H */
