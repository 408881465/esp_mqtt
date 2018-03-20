#ifndef __NTP_H__
#define __NTP_H__
#include <sys/time.h>

#define NTP_TIMEOUT_MS 5000

typedef struct {
	uint8 options;
	uint8 stratum;
	uint8 poll;
	uint8 precision;
	uint32 root_delay;
	uint32 root_disp;
	uint32 ref_id;
	uint8 ref_time[8];
	uint8 orig_time[8];
	uint8 recv_time[8];
	uint8 trans_time[8];
} ntp_t;

void ntp_set_server(uint8_t *ntp_server);
bool ntp_sync_done();
void ntp_get_time();
void get_cur_time(struct timeval *tv);
void set_timezone(int16_t timezone);
uint8_t *get_timestr();
uint8_t *get_weekday();

void set_time_local(uint16_t h, uint16_t m, uint16_t s);
bool set_weekday_local(char *day);

#endif
