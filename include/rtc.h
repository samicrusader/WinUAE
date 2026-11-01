
struct rtc_msm_data
{
	uae_u8 clock_control_d;
	uae_u8 clock_control_e;
	uae_u8 clock_control_f;
	int delayed_write;
	bool yearmode;
	// guest time writes (rtc.cpp): clock = time(0) + cs_rtc_adjust + delta
	int delta;
	int shadow_delta;
	uae_u8 shadow[13];
	bool shadow_valid;
};

#define RF5C01A_RAM_SIZE 16
struct rtc_ricoh_data
{
	uae_u8 clock_control_d;
	uae_u8 clock_control_e;
	uae_u8 clock_control_f;
	uae_u8 rtc_memory[RF5C01A_RAM_SIZE], rtc_alarm[RF5C01A_RAM_SIZE];
	int delayed_write;
	// guest time writes (rtc.cpp): clock = time(0) + cs_rtc_adjust + delta
	int delta;
	int shadow_delta;
	uae_u8 shadow[13];
	bool shadow_valid;
};

uae_u8 get_clock_msm(struct rtc_msm_data *data, int addr, struct tm *ct);
bool put_clock_msm(struct rtc_msm_data *data, int addr, uae_u8 v);
time_t rtc_msm_time(const struct rtc_msm_data *data);

uae_u8 get_clock_ricoh(struct rtc_ricoh_data *data, int addr, struct tm *ct);
void put_clock_ricoh(struct rtc_ricoh_data *data, int addr, uae_u8 v);
time_t rtc_ricoh_time(const struct rtc_ricoh_data *data);
